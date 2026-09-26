#include "vantage.h"
#include <string.h>

/* Binary tree export for graphical front ends (see docs/VANTAGE.md).
 * All integers are little-endian. Records follow inventory order; a parent
 * index equal to the entry count denotes the scanned root. */
static void encode_u64(unsigned char *out, uint64_t v) {
    for (unsigned j = 0; j < 8; ++j) {
        out[j] = (unsigned char)(v >> (8 * j));
    }
}

static void encode_u32(unsigned char *out, uint32_t v) {
    for (unsigned j = 0; j < 4; ++j) {
        out[j] = (unsigned char)(v >> (8 * j));
    }
}

static void put_u32(FILE *out, uint32_t v) {
    unsigned char b[4];
    encode_u32(b, v);
    fwrite(b, 1, sizeof(b), out);
}

static void put_u64(FILE *out, uint64_t v) {
    unsigned char b[8];
    encode_u64(b, v);
    fwrite(b, 1, sizeof(b), out);
}

static void put_text(FILE *out, const char *s) {
    size_t n = strlen(s);
    put_u32(out, (uint32_t)n);
    fwrite(s, 1, n, out);
}

void vantage_export(const VantageView *v, FILE *out) {
    const VSResult *r = v->result;
    const VSInventory *inv = v->inventory;
    fwrite("SVX1", 1, 4, out);
    put_u32(out, (uint32_t)r->completion);
    put_u32(out, (uint32_t)v->config->size);
    put_u64(out, r->scan_ns);
    put_u64(out, r->bytes);
    put_u64(out, r->unique_bytes);
    put_u64(out, r->files);
    put_u64(out, r->directories);
    put_u64(out, r->unknown_sizes);
    put_u64(out, r->counters.permission_errors);
    put_u64(out, r->counters.errors);
    put_text(out, v->config->root);
    put_text(out, r->reason);
    put_text(out, vs_method_name(v->config->method));
    put_u64(out, (uint64_t)inv->count);
    unsigned char buffer[64 * 1024];
    size_t used = 0;
    for (size_t j = 0; j < inv->count; ++j) {
        const VSEntry *e = inv->entries + j;
        size_t length = 29 + (size_t)e->name_length;
        if (length > sizeof(buffer) - used) {
            if (fwrite(buffer, 1, used, out) != used) {
                return;
            }
            used = 0;
        }
        unsigned char record[29];
        unsigned char *destination = length <= sizeof(buffer) ? buffer + used : record;
        encode_u64(destination, e->parent_index);
        destination[8] = (uint8_t)e->kind;
        encode_u64(destination + 9, vantage_bytes(v, j));
        encode_u64(destination + 17, vantage_unknown(v, j));
        encode_u32(destination + 25, e->name_length);
        if (length > sizeof(buffer)) {
            if (fwrite(record, 1, sizeof(record), out) != sizeof(record) ||
                fwrite(inv->names + e->name_offset, 1, e->name_length, out) != e->name_length) {
                return;
            }
        } else {
            memcpy(destination + sizeof(record), inv->names + e->name_offset, e->name_length);
            used += length;
        }
    }
    fwrite(buffer, 1, used, out);
}
