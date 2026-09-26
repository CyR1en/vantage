#include "scan.h"
#include <errno.h>
#include <string.h>

static size_t grow(size_t old, size_t need, size_t unit) {
    size_t n = old ? old : 256;
    while (n < need) {
        if (n > SIZE_MAX / 2) {
            vs_die("vector capacity overflow");
        }
        n *= 2;
    }
    if (unit && n > SIZE_MAX / unit) {
        vs_die("vector byte size overflow");
    }
    return n;
}

void vs_collector_init(VSCollector *c) {
    memset(c, 0, sizeof(*c));
}

void vs_collector_destroy(VSCollector *c) {
    vs_free(c->entries);
    vs_free(c->names);
    memset(c, 0, sizeof(*c));
}

static void checked_add(uint64_t *a, uint64_t n) {
    if (!vs_add_u64(a, n)) {
        vs_die("64-bit accounting overflow");
    }
}

void vs_collect(VSCollector *c, VSEntry e, const void *name) {
    e.valid &= VS_REQUIRED | VS_VALID_SIZE;
    if (e.kind != 1) {
        e.size = 0;
        e.valid |= VS_VALID_SIZE;
    }
    if (!(e.valid & VS_VALID_SIZE)) {
        e.size = 0; /* validity, not this placeholder, defines unknown */
    }
    e.parent_index = VS_NO_INDEX;
    e.subtree_bytes = 0;
    e.subtree_unknown = 0;
    if (e.kind == 1) {
        ++c->files;
        if (e.valid & VS_VALID_SIZE) {
            checked_add(&c->bytes, e.size);
        } else {
            ++c->unknown_sizes;
        }
    } else if (e.kind == 2) {
        ++c->directories;
    }
    if (c->count == c->capacity) {
        c->capacity = grow(c->capacity, c->count + 1, sizeof(VSEntry));
        c->entries = vs_realloc(c->entries, c->capacity * sizeof(VSEntry));
    }
    if (c->names_size > SIZE_MAX - e.name_length - 1) {
        vs_die("name arena overflow");
    }
    size_t need = c->names_size + e.name_length + 1;
    if (need > c->names_capacity) {
        c->names_capacity = grow(c->names_capacity, need, 1);
        c->names = vs_realloc(c->names, c->names_capacity);
    }
    e.name_offset = c->names_size;
    memcpy(c->names + c->names_size, name, e.name_length);
    c->names[need - 1] = 0;
    c->names_size = need;
    c->entries[c->count++] = e;
}

static void account_unique(VSResult *r, const VSEntry *o) {
    ++r->unique_objects;
    if (o->kind == 1) {
        ++r->unique_files;
        if (o->valid & VS_VALID_SIZE) {
            checked_add(&r->unique_bytes, o->size);
        }
    }
}

static void conflicting_object(VSResult *r, const VSEntry *a, const VSEntry *b) {
    if (a->kind != b->kind || a->valid != b->valid || a->size != b->size) {
        ++r->counters.identity_races;
        vs_failure(r, VS_PARTIAL, ESTALE, "inconsistent metadata for one object identity");
    }
}

static size_t hash_capacity(size_t n) {
    size_t cap = 16;
    if (n > SIZE_MAX / 2) {
        vs_die("hash table overflow");
    }
    while (cap < n * 2) {
        if (cap > SIZE_MAX / 2) {
            vs_die("hash table overflow");
        }
        cap *= 2;
    }
    return cap;
}

static void reduce_entries(const VSInventory *i, VSResult *r) {
    size_t cap = hash_capacity(i->count);
    size_t *slots = vs_calloc(cap, sizeof(*slots));
    for (size_t j = 0; j < i->count; ++j) {
        const VSEntry *entry = i->entries + j;
        size_t at = (size_t)vs_mix(entry->object_id ^ vs_mix(entry->device)) & (cap - 1);
        while (slots[at]) {
            const VSEntry *previous = i->entries + slots[at] - 1;
            if (entry->device == previous->device && entry->object_id == previous->object_id) {
                conflicting_object(r, previous, entry);
                break;
            }
            at = (at + 1) & (cap - 1);
        }
        if (!slots[at]) {
            slots[at] = j + 1;
            account_unique(r, entry);
        }
    }
    vs_free(slots);
}

static void build_graph(VSInventory *i, VSResult *r) {
    size_t n = i->count, cap = hash_capacity((size_t)r->directories);
    size_t *slots = vs_calloc(cap, sizeof(*slots));
    uint64_t *remaining = vs_calloc(n + 1, sizeof(*remaining));
    /* Only directories enter the bottom-up queue; files are accounted inline. */
    size_t *queue = vs_calloc((size_t)r->directories, sizeof(*queue));
    r->graph_valid = true;
    for (size_t j = 0; j < n; ++j) {
        VSEntry *e = i->entries + j;
        if (e->kind != 2) {
            continue;
        }
        size_t at = (size_t)vs_mix(e->object_id ^ vs_mix(e->device)) & (cap - 1);
        while (slots[at]) {
            const VSEntry *p = i->entries + slots[at] - 1;
            if (p->device == e->device && p->object_id == e->object_id) {
                break;
            }
            at = (at + 1) & (cap - 1);
        }
        if (slots[at] || (e->device == r->root_device && e->object_id == r->root_id)) {
            ++r->counters.duplicates;
            r->graph_valid = false;
        } else {
            slots[at] = j + 1;
        }
    }
    uint64_t root_bytes = 0, root_unknown = 0;
    for (size_t j = 0; j < n; ++j) {
        VSEntry *e = i->entries + j;
        size_t parent = n;
        if (e->parent_id != r->root_id || e->device != r->root_device) {
            size_t at = (size_t)vs_mix(e->parent_id ^ vs_mix(e->device)) & (cap - 1);
            while (slots[at]) {
                VSEntry *p = i->entries + slots[at] - 1;
                if (p->device == e->device && p->object_id == e->parent_id) {
                    break;
                }
                at = (at + 1) & (cap - 1);
            }
            if (!slots[at]) {
                ++r->counters.missing_parents;
                r->graph_valid = false;
                continue;
            }
            parent = slots[at] - 1;
        }
        e->parent_index = parent;
        if (e->kind == 2) {
            ++remaining[parent];
        } else if (e->kind == 1) {
            uint64_t *bytes = parent == n ? &root_bytes : &i->entries[parent].subtree_bytes;
            uint64_t *unknown = parent == n ? &root_unknown : &i->entries[parent].subtree_unknown;
            if (e->valid & VS_VALID_SIZE) {
                checked_add(bytes, e->size);
            } else {
                ++*unknown;
            }
        }
    }
    size_t head = 0, tail = 0, done = 0;
    for (size_t j = 0; j < n; ++j) {
        if (i->entries[j].kind == 2 && !remaining[j]) {
            queue[tail++] = j;
        }
    }
    while (head < tail) {
        size_t j = queue[head++];
        VSEntry *e = i->entries + j;
        ++done;
        if (e->parent_index == VS_NO_INDEX) {
            continue;
        }
        size_t parent = (size_t)e->parent_index;
        uint64_t *bytes = parent == n ? &root_bytes : &i->entries[parent].subtree_bytes;
        uint64_t *unknown = parent == n ? &root_unknown : &i->entries[parent].subtree_unknown;
        checked_add(bytes, e->subtree_bytes);
        checked_add(unknown, e->subtree_unknown);
        if (!remaining[parent]) {
            r->graph_valid = false;
            continue;
        }
        if (--remaining[parent] == 0 && parent != n) {
            queue[tail++] = parent;
        }
    }
    if (done != r->directories || root_bytes != r->bytes || root_unknown != r->unknown_sizes) {
        r->graph_valid = false;
    }
    r->graph_root_bytes = root_bytes;
    if (!r->graph_valid) {
        vs_failure(r, VS_PARTIAL, EINVAL,
                   "directory graph has duplicate identities, missing parents, or a cycle");
    }
    vs_free(slots);
    vs_free(remaining);
    vs_free(queue);
}

void vs_collect_finalize(VSCollector *cs, size_t workers, VSResult *r, VSInventory *i) {
    memset(i, 0, sizeof(*i));
    i->root_device = r->root_device;
    i->root_id = r->root_id;
    size_t n = 0, names = 0;
    for (size_t w = 0; w < workers; ++w) {
        VSCollector *c = cs + w;
        if (c->count > SIZE_MAX - n || c->names_size > SIZE_MAX - names) {
            vs_die("collector merge overflow");
        }
        n += c->count;
        names += c->names_size;
        r->files += c->files;
        r->directories += c->directories;
        checked_add(&r->bytes, c->bytes);
        r->unknown_sizes += c->unknown_sizes;
    }
    r->entries = n;
    i->entries = vs_calloc(n, sizeof(*i->entries));
    i->names = vs_alloc(names);
    i->count = n;
    i->names_size = names;
    size_t off = 0, name_off = 0;
    for (size_t w = 0; w < workers; ++w) {
        VSCollector *c = cs + w;
        if (c->names_size) {
            memcpy(i->names + name_off, c->names, c->names_size);
        }
        for (size_t j = 0; j < c->count; ++j) {
            i->entries[off + j] = c->entries[j];
            i->entries[off + j].name_offset += name_off;
        }
        name_off += c->names_size;
        off += c->count;
        vs_collector_destroy(c);
    }
    reduce_entries(i, r);
    if (r->unknown_sizes) {
        vs_failure(r, VS_PARTIAL, ENODATA, "one or more requested sizes are unknown");
    }
    build_graph(i, r);
    if (r->counters.errors || r->counters.unknown_attributes || r->counters.duplicates) {
        vs_failure(r, VS_PARTIAL, r->error_code, "scan encountered errors; see counters");
    }
}

void vs_inventory_destroy(VSInventory *i) {
    vs_free(i->entries);
    vs_free(i->names);
    memset(i, 0, sizeof(*i));
}
