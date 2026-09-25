#include "scanbench.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static size_t grow(size_t old, size_t need, size_t unit) {
    size_t n = old ? old : 256;
    while (n < need) { if (n > SIZE_MAX / 2) sb_die("vector capacity overflow"); n *= 2; }
    if (unit && n > SIZE_MAX / unit) sb_die("vector byte size overflow");
    return n;
}
void sb_collector_init(SBCollector *c, bool retain) { memset(c, 0, sizeof(*c)); c->retain = retain; }
void sb_collector_destroy(SBCollector *c) {
    sb_free(c->entries); sb_free(c->names); sb_free(c->objects); memset(c, 0, sizeof(*c));
}
static void encode64(unsigned char *p, uint64_t x) { for (unsigned j = 0; j < 8; ++j) p[j] = (unsigned char)(x >> (j * 8)); }
static void checked_add(uint64_t *a, uint64_t n) { if (!sb_add_u64(a, n)) sb_die("64-bit accounting overflow"); }
void sb_collect(SBCollector *c, const SBConfig *cfg, SBEntry e, const void *name) {
    e.valid &= SB_REQUIRED | SB_VALID_SIZE;
    if (cfg->task == SB_ENUMERATE || e.kind != 1) { e.size = 0; e.valid |= SB_VALID_SIZE; }
    if (!(e.valid & SB_VALID_SIZE)) e.size = 0; /* validity, not this placeholder, defines unknown */
    e.parent_index = SB_NO_INDEX; e.subtree_bytes = 0; e.subtree_unknown = 0; e.reserved = 0;
    unsigned char key[56];
    encode64(key, e.device); encode64(key + 8, e.object_id); encode64(key + 16, e.parent_id);
    encode64(key + 24, e.kind); encode64(key + 32, e.valid); encode64(key + 40, e.size); encode64(key + 48, e.name_length);
    uint64_t h = sb_hash_bytes(key, sizeof(key), 0);
    h = sb_hash_bytes(name, e.name_length, h);
    c->digest_a += h; c->digest_b += sb_mix(h ^ UINT64_C(0x243f6a8885a308d3)); c->digest_c += h * h;
    if (e.kind == 1) { ++c->files; if (e.valid & SB_VALID_SIZE) checked_add(&c->bytes, e.size); else ++c->unknown_sizes; }
    else if (e.kind == 2) ++c->directories;
    else if (e.kind == 5) ++c->symlinks;
    else ++c->other;
    if (c->retain) {
        if (c->count == c->capacity) { c->capacity = grow(c->capacity, c->count + 1, sizeof(SBEntry)); c->entries = sb_realloc(c->entries, c->capacity * sizeof(SBEntry)); }
        if (c->names_size > SIZE_MAX - e.name_length - 1) sb_die("name arena overflow");
        size_t need = c->names_size + e.name_length + 1;
        if (need > c->names_capacity) { c->names_capacity = grow(c->names_capacity, need, 1); c->names = sb_realloc(c->names, c->names_capacity); }
        e.name_offset = c->names_size;
        memcpy(c->names + c->names_size, name, e.name_length); c->names[need - 1] = 0; c->names_size = need;
        c->entries[c->count++] = e;
    } else {
        if (c->objects_count == c->objects_capacity) { c->objects_capacity = grow(c->objects_capacity, c->objects_count + 1, sizeof(SBObject)); c->objects = sb_realloc(c->objects, c->objects_capacity * sizeof(SBObject)); }
        c->objects[c->objects_count++] = (SBObject){e.device, e.object_id, e.size, e.kind, e.valid};
        ++c->count;
    }
}
static int object_compare(const void *a, const void *b) {
    const SBObject *x = a, *y = b;
    if (x->device != y->device) return x->device < y->device ? -1 : 1;
    if (x->object_id != y->object_id) return x->object_id < y->object_id ? -1 : 1;
    return 0;
}
static void account_unique(SBResult *r, const SBObject *o) {
    ++r->unique_objects;
    if (o->kind == 1) { ++r->unique_files; if (o->valid & SB_VALID_SIZE) checked_add(&r->unique_bytes, o->size); }
}
static void conflicting_object(SBResult *r, const SBObject *a, const SBObject *b) {
    if (a->kind != b->kind || a->valid != b->valid || a->size != b->size) {
        ++r->counters.identity_races;
        sb_failure(r, SB_PARTIAL, ESTALE, "inconsistent metadata for one object identity");
    }
}
static size_t hash_capacity(size_t n) {
    size_t cap = 16;
    if (n > SIZE_MAX / 2) sb_die("hash table overflow");
    while (cap < n * 2) { if (cap > SIZE_MAX / 2) sb_die("hash table overflow"); cap *= 2; }
    return cap;
}
static void reduce_objects(SBObject *objects, size_t n, SBReduce mode, SBResult *r) {
    if (mode == SB_REDUCE_SORT) {
        if (n) qsort(objects, n, sizeof(*objects), object_compare);
        for (size_t j = 0; j < n; ++j) {
            if (!j || object_compare(objects + j - 1, objects + j)) account_unique(r, objects + j);
            else conflicting_object(r, objects + j - 1, objects + j);
        }
    } else {
        size_t cap = hash_capacity(n);
        size_t *slots = sb_calloc(cap, sizeof(*slots));
        for (size_t j = 0; j < n; ++j) {
            const SBObject *o = objects + j;
            size_t at = (size_t)sb_mix(o->object_id ^ sb_mix(o->device)) & (cap - 1);
            while (slots[at] && object_compare(objects + slots[at] - 1, o)) at = (at + 1) & (cap - 1);
            if (!slots[at]) { slots[at] = j + 1; account_unique(r, o); }
            else conflicting_object(r, objects + slots[at] - 1, o);
        }
        sb_free(slots);
    }
}
static void build_graph(SBInventory *i, SBResult *r) {
    size_t n = i->count, cap = hash_capacity((size_t)r->directories);
    size_t *slots = sb_calloc(cap, sizeof(*slots));
    uint64_t *remaining = sb_calloc(n + 1, sizeof(*remaining));
    /* Only directories enter the bottom-up queue; files are accounted inline. */
    size_t *queue = sb_calloc((size_t)r->directories, sizeof(*queue));
    r->graph_valid = true;
    for (size_t j = 0; j < n; ++j) {
        SBEntry *e = i->entries + j;
        if (e->kind != 2) continue;
        size_t at = (size_t)sb_mix(e->object_id ^ sb_mix(e->device)) & (cap - 1);
        while (slots[at]) {
            const SBEntry *p = i->entries + slots[at] - 1;
            if (p->device == e->device && p->object_id == e->object_id) break;
            at = (at + 1) & (cap - 1);
        }
        if (slots[at] || (e->device == r->root_device && e->object_id == r->root_id)) {
            ++r->counters.duplicates; r->graph_valid = false;
        } else slots[at] = j + 1;
    }
    uint64_t root_bytes = 0, root_unknown = 0;
    for (size_t j = 0; j < n; ++j) {
        SBEntry *e = i->entries + j;
        size_t parent = n;
        if (e->parent_id != r->root_id || e->device != r->root_device) {
            size_t at = (size_t)sb_mix(e->parent_id ^ sb_mix(e->device)) & (cap - 1);
            while (slots[at]) {
                SBEntry *p = i->entries + slots[at] - 1;
                if (p->device == e->device && p->object_id == e->parent_id) break;
                at = (at + 1) & (cap - 1);
            }
            if (!slots[at]) { ++r->counters.missing_parents; r->graph_valid = false; continue; }
            parent = slots[at] - 1;
        }
        e->parent_index = parent;
        if (e->kind == 2) ++remaining[parent];
        else if (e->kind == 1) {
            uint64_t *bytes = parent == n ? &root_bytes : &i->entries[parent].subtree_bytes;
            uint64_t *unknown = parent == n ? &root_unknown : &i->entries[parent].subtree_unknown;
            if (e->valid & SB_VALID_SIZE) checked_add(bytes, e->size); else ++*unknown;
        }
    }
    size_t head = 0, tail = 0, done = 0;
    for (size_t j = 0; j < n; ++j) if (i->entries[j].kind == 2 && !remaining[j]) queue[tail++] = j;
    while (head < tail) {
        size_t j = queue[head++]; SBEntry *e = i->entries + j; ++done;
        if (e->parent_index == SB_NO_INDEX) continue;
        size_t parent = (size_t)e->parent_index;
        uint64_t *bytes = parent == n ? &root_bytes : &i->entries[parent].subtree_bytes;
        uint64_t *unknown = parent == n ? &root_unknown : &i->entries[parent].subtree_unknown;
        checked_add(bytes, e->subtree_bytes); checked_add(unknown, e->subtree_unknown);
        if (!remaining[parent]) { r->graph_valid = false; continue; }
        if (--remaining[parent] == 0 && parent != n) queue[tail++] = parent;
    }
    if (done != r->directories || root_bytes != r->bytes || root_unknown != r->unknown_sizes) r->graph_valid = false;
    r->graph_root_bytes = root_bytes;
    if (!r->graph_valid) sb_failure(r, SB_PARTIAL, EINVAL, "directory graph has duplicate identities, missing parents, or a cycle");
    sb_free(slots); sb_free(remaining); sb_free(queue);
}
void sb_collect_finalize(SBCollector *cs, size_t workers, const SBConfig *cfg,
                         SBResult *r, SBInventory *i) {
    memset(i, 0, sizeof(*i));
    i->root_device = r->root_device; i->root_id = r->root_id; i->task = cfg->task; i->size = cfg->size;
    size_t n = 0, names = 0;
    for (size_t w = 0; w < workers; ++w) {
        SBCollector *c = cs + w;
        if (c->count > SIZE_MAX - n || c->names_size > SIZE_MAX - names) sb_die("collector merge overflow");
        n += c->count; names += c->names_size;
        r->files += c->files; r->directories += c->directories; r->symlinks += c->symlinks; r->other += c->other;
        checked_add(&r->bytes, c->bytes); r->unknown_sizes += c->unknown_sizes;
        r->digest_a += c->digest_a; r->digest_b += c->digest_b; r->digest_c += c->digest_c;
    }
    r->entries = n;
    SBObject *objects = sb_calloc(n, sizeof(*objects));
    bool retain = cfg->task == SB_TREE || cfg->keep_manifest;
    if (retain) { i->entries = sb_calloc(n, sizeof(*i->entries)); i->names = sb_alloc(names); i->count = n; i->names_size = names; }
    size_t off = 0, name_off = 0;
    for (size_t w = 0; w < workers; ++w) {
        SBCollector *c = cs + w;
        if (c->retain) {
            if (retain) {
                if (c->names_size) memcpy(i->names + name_off, c->names, c->names_size);
                for (size_t j = 0; j < c->count; ++j) { i->entries[off + j] = c->entries[j]; i->entries[off + j].name_offset += name_off; }
                name_off += c->names_size;
            }
            for (size_t j = 0; j < c->count; ++j) {
                SBEntry *e = c->entries + j;
                objects[off + j] = (SBObject){e->device, e->object_id, e->size, e->kind, e->valid};
            }
        } else if (c->count) memcpy(objects + off, c->objects, c->count * sizeof(*objects));
        off += c->count; sb_collector_destroy(c);
    }
    reduce_objects(objects, n, cfg->reduce, r); sb_free(objects);
    if (r->unknown_sizes) sb_failure(r, SB_PARTIAL, ENODATA, "one or more requested sizes are unknown");
    if (cfg->task == SB_TREE) build_graph(i, r);
    if (r->counters.errors || r->counters.unknown_attributes || r->counters.duplicates)
        sb_failure(r, SB_PARTIAL, r->error_code, "scan encountered errors; see counters");
}
void sb_inventory_destroy(SBInventory *i) { sb_free(i->entries); sb_free(i->names); memset(i, 0, sizeof(*i)); }

typedef struct { SBEntry entry; const unsigned char *name; size_t old_index; } SortEntry;
static int entry_compare(const void *a, const void *b) {
    const SortEntry *x = a, *y = b;
#define CMP(f) if (x->entry.f != y->entry.f) return x->entry.f < y->entry.f ? -1 : 1
    CMP(device); CMP(parent_id);
    size_t n = x->entry.name_length < y->entry.name_length ? x->entry.name_length : y->entry.name_length;
    int c = n ? memcmp(x->name, y->name, n) : 0; if (c) return c;
    CMP(name_length); CMP(object_id); CMP(kind); CMP(valid); CMP(size); CMP(subtree_bytes); CMP(subtree_unknown);
#undef CMP
    return 0;
}
void sb_inventory_sort(SBInventory *i) {
    SortEntry *v = sb_calloc(i->count, sizeof(*v));
    size_t *inverse = sb_calloc(i->count, sizeof(*inverse));
    for (size_t j = 0; j < i->count; ++j) { v[j].entry = i->entries[j]; v[j].name = i->names + i->entries[j].name_offset; v[j].old_index = j; }
    if (i->count) qsort(v, i->count, sizeof(*v), entry_compare);
    for (size_t j = 0; j < i->count; ++j) inverse[v[j].old_index] = j;
    for (size_t j = 0; j < i->count; ++j) {
        i->entries[j] = v[j].entry;
        if (i->entries[j].parent_index < i->count) i->entries[j].parent_index = inverse[i->entries[j].parent_index];
    }
    sb_free(v); sb_free(inverse);
}
bool sb_inventory_equal(SBInventory *a, SBInventory *b, char *why, size_t n) {
    if (a->root_device != b->root_device || a->root_id != b->root_id || a->task != b->task || a->size != b->size) { snprintf(why, n, "root identity or task contract differs"); return false; }
    if (a->count != b->count) { snprintf(why, n, "entry count differs: %zu versus %zu", a->count, b->count); return false; }
    sb_inventory_sort(a); sb_inventory_sort(b);
    for (size_t j = 0; j < a->count; ++j) {
        SortEntry x = {.entry = a->entries[j], .name = a->names + a->entries[j].name_offset};
        SortEntry y = {.entry = b->entries[j], .name = b->names + b->entries[j].name_offset};
        if (entry_compare(&x, &y)) { snprintf(why, n, "normalized entry %zu differs (object %" PRIu64 "/%" PRIu64 ")", j, x.entry.object_id, y.entry.object_id); return false; }
        if (j) {
            SBEntry *prev = a->entries + j - 1, *cur = a->entries + j;
            if (prev->device == cur->device && prev->parent_id == cur->parent_id && prev->name_length == cur->name_length &&
                !memcmp(a->names + prev->name_offset, a->names + cur->name_offset, cur->name_length)) {
                snprintf(why, n, "duplicate directory-entry edge at %zu", j); return false;
            }
        }
    }
    snprintf(why, n, "exact normalized manifests match"); return true;
}
