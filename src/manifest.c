#include "scanbench.h"
#include <errno.h>
#include <inttypes.h>
#include <string.h>

static void put64(unsigned char *p, uint64_t x) { for (unsigned j = 0; j < 8; ++j) p[j] = (unsigned char)(x >> (8 * j)); }
static uint64_t get64(const unsigned char *p) { uint64_t n = 0; for (unsigned j = 0; j < 8; ++j) n |= (uint64_t)p[j] << (8 * j); return n; }
static void put32(unsigned char *p, uint32_t x) { for (unsigned j = 0; j < 4; ++j) p[j] = (unsigned char)(x >> (8 * j)); }
static uint32_t get32(const unsigned char *p) { uint32_t n = 0; for (unsigned j = 0; j < 4; ++j) n |= (uint32_t)p[j] << (8 * j); return n; }
/* SBINV001: header 48 bytes, then 60-byte little-endian records and raw names.
 * This is an inventory/collector replay format, not a raw Darwin batch capture. */
bool sb_manifest_write(int fd, const SBInventory *i) {
    unsigned char h[48]; memcpy(h, "SBINV001", 8); put64(h + 8, i->count); put64(h + 16, i->names_size);
    put64(h + 24, i->root_device); put64(h + 32, i->root_id); put32(h + 40, i->task); put32(h + 44, i->size);
    if (!sb_write_all(fd, h, sizeof(h))) return false;
    for (size_t j = 0; j < i->count; ++j) {
        const SBEntry *e = i->entries + j; unsigned char b[60];
        put64(b, e->device); put64(b + 8, e->object_id); put64(b + 16, e->parent_id); put64(b + 24, e->size);
        put64(b + 32, e->subtree_bytes); put64(b + 40, e->subtree_unknown);
        put32(b + 48, e->kind); put32(b + 52, e->valid); put32(b + 56, e->name_length);
        if (!sb_write_all(fd, b, sizeof(b)) || !sb_write_all(fd, i->names + e->name_offset, e->name_length)) return false;
    }
    return true;
}
bool sb_manifest_read(int fd, SBInventory *i, uint64_t limit) {
    memset(i, 0, sizeof(*i)); unsigned char h[48];
    if (!sb_read_all(fd, h, sizeof(h)) || memcmp(h, "SBINV001", 8)) return false;
    uint64_t count = get64(h + 8), names = get64(h + 16);
    if (get32(h + 40) > SB_TREE || get32(h + 44) > SB_ALLOCATED) return false;
    i->root_device = get64(h + 24); i->root_id = get64(h + 32);
    i->task = (SBTask)get32(h + 40); i->size = (SBSize)get32(h + 44);
    if (count > SIZE_MAX / sizeof(SBEntry) || names > SIZE_MAX || names < count) return false;
    uint64_t entry_bytes = count * sizeof(SBEntry);
    if (entry_bytes > limit || names > limit - entry_bytes) return false;
    i->entries = sb_calloc((size_t)count, sizeof(*i->entries)); i->names = sb_alloc((size_t)names);
    i->count = (size_t)count; i->names_size = (size_t)names;
    size_t off = 0;
    for (size_t j = 0; j < i->count; ++j) {
        unsigned char b[60]; if (!sb_read_all(fd, b, sizeof(b))) goto fail;
        SBEntry *e = i->entries + j;
        e->device = get64(b); e->object_id = get64(b + 8); e->parent_id = get64(b + 16); e->size = get64(b + 24);
        e->subtree_bytes = get64(b + 32); e->subtree_unknown = get64(b + 40);
        e->kind = get32(b + 48); e->valid = get32(b + 52); e->name_length = get32(b + 56); e->name_offset = off; e->parent_index = SB_NO_INDEX;
        if (off >= names || e->name_length >= names - off || e->kind > 9 || (e->valid & ~(SB_REQUIRED | SB_VALID_SIZE))) goto fail;
        if (!sb_read_all(fd, i->names + off, e->name_length)) goto fail;
        if (memchr(i->names + off, 0, e->name_length) || memchr(i->names + off, '/', e->name_length)) goto fail;
        off += e->name_length; i->names[off++] = 0;
    }
    if (off != names) goto fail;
    return true;
fail:
    sb_inventory_destroy(i); return false;
}
void sb_manifest_json(FILE *f, const SBInventory *i) {
    for (size_t j = 0; j < i->count; ++j) {
        const SBEntry *e = i->entries + j;
        fprintf(f, "{\"device\":\"%" PRIu64 "\",\"id\":\"%" PRIu64 "\",\"parent\":\"%" PRIu64 "\",\"kind\":%u,\"valid\":%u,\"name_hex\":", e->device, e->object_id, e->parent_id, e->kind, e->valid);
        sb_json_hex(f, i->names + e->name_offset, e->name_length);
        if (e->valid & SB_VALID_SIZE) fprintf(f, ",\"size\":%" PRIu64, e->size); else fputs(",\"size\":null", f);
        fprintf(f, ",\"subtree_bytes\":%" PRIu64 ",\"subtree_unknown\":%" PRIu64 "}\n", e->subtree_bytes, e->subtree_unknown);
    }
}
