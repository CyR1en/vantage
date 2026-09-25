#include "scanbench.h"
#include "attrs.h"
#include "wire.h"
#include "fixtures/attrs/captured.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static SBConfig config(void) { SBConfig c = {0}; c.task = SB_TREE; c.size = SB_LOGICAL; c.keep_manifest = true; return c; }
static SBEntry entry(uint64_t id, uint64_t parent, unsigned kind, uint64_t size) {
    return (SBEntry){.device = 42, .object_id = id, .parent_id = parent, .kind = kind, .size = size, .valid = SB_REQUIRED | SB_VALID_SIZE};
}
static void add(SBCollector *c, SBConfig *cfg, SBEntry e, const char *name) { e.name_length = (uint32_t)strlen(name); sb_collect(c, cfg, e, name); }
static void captured_parser_tests(void) {
    const char *names[] = {"symlink", "file", "dir", "fifo"};
    const uint32_t kinds[] = {5, 1, 2, 7};
    const uint64_t ids[] = {111222590, 111222588, 111222589, 111222591};
    const uint64_t logical[] = {4, 7, 0, 0};
    unsigned total = 0;
    for (size_t i = 0; i < sizeof(captured_attrs) / sizeof(*captured_attrs); ++i) {
        const CapturedAttrs *f = captured_attrs + i;
        SBConfig cfg = config(); cfg.extra_uuid = f->uuid; cfg.omit_objtype = f->omit; cfg.pack_invalid = f->pack;
        SBAttrSpec spec = sb_attr_spec(&cfg, false);
        size_t offset = 0, count = f->denied ? 2 : 4;
        for (size_t j = 0; j < count; ++j) {
            SBAttrRecord r;
            bool decoded = sb_attr_decode(f->bytes + offset, f->length - offset, spec, SB_LOGICAL, &r);
            if (!decoded) fprintf(stderr, "captured record rejected: %s[%zu]\n", f->name, j);
            CHECK(decoded);
            const char *name = f->denied ? j ? "denied-dir" : "denied-file" : names[j];
            CHECK(r.name && !strcmp(r.name, name) && r.entry.name_length == strlen(name));
            bool denied = f->denied && (f->uuid || f->omit);
            CHECK(r.error == (denied ? EACCES : 0));
            if (denied) {
                CHECK(!r.entry.valid && !r.has_logical && !r.has_mountstatus);
            } else {
                uint32_t kind = f->denied ? j ? 2 : 1 : kinds[j];
                uint64_t id = f->denied ? j ? 111222690 : 111222689 : ids[j];
                CHECK((r.entry.valid & (SB_VALID_ID | SB_VALID_DEVICE)) == (SB_VALID_ID | SB_VALID_DEVICE));
                CHECK(r.entry.object_id == id && r.entry.device == 16777230);
                CHECK((r.entry.valid & SB_VALID_KIND) == (f->omit ? 0 : SB_VALID_KIND));
                if (!f->omit) CHECK(r.entry.kind == kind);
                CHECK(r.has_mountstatus == (kind == 2) && r.has_logical == (kind != 2));
                if (kind != 2) CHECK(r.logical == (f->denied ? 7 : logical[j]));
                if (!f->omit) CHECK(r.entry.size == (kind == 1 ? 7 : 0));
            }
            CHECK(r.record_size && r.record_size <= f->length - offset);
            for (size_t n = 0; n < r.record_size; ++n) {
                SBAttrRecord truncated;
                CHECK(!sb_attr_decode(f->bytes + offset, n, spec, SB_LOGICAL, &truncated));
            }
            offset += r.record_size; ++total;
        }
        CHECK(offset == f->length);
    }
    CHECK(total == 48);
    puts("unit: 48 independently captured macOS attribute records and all truncations passed");
}
static void put32(unsigned char *bytes, size_t offset, uint32_t value) { memcpy(bytes + offset, &value, 4); }
static uint32_t get32(const unsigned char *bytes, size_t offset) { uint32_t value; memcpy(&value, bytes + offset, 4); return value; }
static void parser_edge_tests(void) {
    SBConfig cfg = config(); cfg.pack_invalid = true;
    SBAttrSpec spec = sb_attr_spec(&cfg, false);
    unsigned char b[512]; SBAttrRecord r;
    const unsigned char *captured = normal_uuid0_omit0_pack1;
    size_t n = get32(captured, 0);
    uint32_t common = get32(captured, 4);
    /* Both type masks, or a type that contradicts its returned mask. */
    memcpy(b, captured, n); put32(b, 12, SB_D_MOUNTSTATUS);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    put32(b, 16, 0);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    memcpy(b, captured, n); put32(b, 40, 2);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    put32(b, 40, 0);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    put32(b, 40, 10);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    /* A poisoned invalid type is never promoted into a valid type. */
    memcpy(b, captured, n); put32(b, 4, common & ~SB_A_OBJTYPE); put32(b, 40, UINT32_MAX);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    CHECK(!(r.entry.valid & SB_VALID_KIND) && !r.entry.kind && r.has_logical && r.logical == 4);
    /* No valid type-specific values: ignore the bounded tail and require enrichment. */
    put32(b, 16, 0); memset(b + 56, 0xff, 8);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    CHECK(!(r.entry.valid & (SB_VALID_KIND | SB_VALID_SIZE)) && !r.has_logical && !r.logical);
    put32(b, 4, common & ~(SB_A_OBJTYPE | SB_A_FLAGS | SB_A_FILEID | SB_A_DEVID));
    memset(b + 36, 0xff, 20);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    CHECK(!r.entry.valid && !r.entry.device && !r.entry.object_id && !r.flags && !r.has_flags);
    /* Names must lie after the parsed common prefix, even for an opaque tail. */
    put32(b, 28, 16); put32(b, 32, 4); memcpy(b + 44, "abc", 4);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    /* Returned ERROR governs its physical presence even when packing is enabled. */
    SBEntry e = entry(21, 8, 1, 19);
    n = wire_encode(b, spec, e, "no-error", spec.common & ~SB_A_ERROR, 0, spec.file, 0, 0, 0);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r) && r.entry.size == 19 && !r.error);
    /* Invalid packed sizes must not become values or signed-length failures. */
    e.size = UINT64_MAX;
    n = wire_encode(b, spec, e, "invalid-size", spec.common, 0, 0, 0, 0, 0);
    memset(b + 28 + get32(b, 28) - 8, 0xff, 8);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    CHECK(!(r.entry.valid & SB_VALID_SIZE) && !r.has_logical && !r.logical);
    /* A common-only enumerable record has no file section to infer. */
    cfg.omit_objtype = true; cfg.task = SB_ENUMERATE; spec = sb_attr_spec(&cfg, false);
    n = wire_encode(b, spec, e, "enumerated", spec.common, 0, 0, 0, 0, 0);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r) && !strcmp(r.name, "enumerated"));
    CHECK(!(r.entry.valid & (SB_VALID_KIND | SB_VALID_SIZE)));
    /* Real EACCES records have zero type masks but retain a bounded name reference. */
    cfg = config(); cfg.pack_invalid = cfg.extra_uuid = true; spec = sb_attr_spec(&cfg, false);
    captured = error_uuid1_omit0_pack1; n = get32(captured, 0);
    memcpy(b, captured, n);
    CHECK(sb_attr_decode(b, n, spec, SB_LOGICAL, &r) && r.error == EACCES && !r.entry.valid);
    for (unsigned trial = 0; trial < 6; ++trial) {
        memcpy(b, captured, n);
        size_t name = 28 + get32(b, 28); uint32_t length = get32(b, 32);
        if (trial == 0) put32(b, 28, UINT32_MAX); /* Backward overlap. */
        if (trial == 1) put32(b, 28, INT32_MAX); /* Beyond the record. */
        if (trial == 2) put32(b, 32, 0);
        if (trial == 3) b[name + length - 1] = 'x';
        if (trial == 4) b[name + 1] = '\0';
        if (trial == 5) b[name + 1] = '/';
        CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r));
    }
    memcpy(b, captured, n); put32(b, 24, 0); put32(b, 4, SB_A_RETURNED | SB_A_ERROR);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r)); /* Success requires a name. */
    memcpy(b, captured, n); put32(b, 4, get32(b, 4) | SB_A_PARENTID);
    CHECK(!sb_attr_decode(b, n, spec, SB_LOGICAL, &r)); /* Unrequested returned attribute. */
    puts("unit: packed validity, ambiguous type, contradictory masks and bounded error names passed");
}
static void captured_mutation_tests(void) {
    uint64_t seed = 5;
    unsigned char corrupt[512];
    for (unsigned trial = 0; trial < 100000; ++trial) {
        const CapturedAttrs *f = captured_attrs + trial % (sizeof(captured_attrs) / sizeof(*captured_attrs));
        SBConfig cfg = config(); cfg.extra_uuid = f->uuid; cfg.omit_objtype = f->omit; cfg.pack_invalid = f->pack;
        size_t n = get32(f->bytes, 0); CHECK(n <= sizeof(corrupt)); memcpy(corrupt, f->bytes, n);
        for (unsigned k = 0; k < 3; ++k) corrupt[sb_random(&seed) % n] ^= (unsigned char)sb_random(&seed);
        SBAttrRecord r; (void)sb_attr_decode(corrupt, n, sb_attr_spec(&cfg, false), SB_LOGICAL, &r);
    }
    puts("unit: 100000 mutations across captured packed/unpacked, omitted-type and error records passed");
}
static void parser_tests(void) {
    SBConfig cfg = config(); cfg.extra_entrycount = true;
    SBEntry e = entry(UINT64_C(0xf000000000000021), UINT64_C(0xe000000000000008), 1, 19);
    unsigned char buffer[512], corrupt[512]; SBAttrRecord record;
    for (unsigned catalog = 0; catalog < 2; ++catalog) for (unsigned pack = 0; pack < 2; ++pack) {
        if (catalog && pack) continue;
        cfg.pack_invalid = pack != 0;
        SBAttrSpec spec = sb_attr_spec(&cfg, catalog != 0);
        size_t length = wire_encode(buffer, spec, e, "alpha", spec.common, 0, spec.file, 0, 0, 0);
        CHECK(sb_attr_decode(buffer, length, spec, SB_LOGICAL, &record));
        CHECK(record.entry.object_id == e.object_id && record.entry.size == 19 && !strcmp(record.name, "alpha"));
        if (catalog) CHECK(record.entry.parent_id == e.parent_id);
        CHECK(record.record_size == length);
        for (size_t n = 0; n < length; ++n) CHECK(!sb_attr_decode(buffer, n, spec, SB_LOGICAL, &record));
        memcpy(corrupt, buffer, length); corrupt[0] = 3; CHECK(!sb_attr_decode(corrupt, length, spec, SB_LOGICAL, &record));
        e.kind = 2;
        length = wire_encode(buffer, spec, e, "directory", spec.common, spec.dir, 0, 0, 0, 0);
        CHECK(sb_attr_decode(buffer, length, spec, SB_LOGICAL, &record));
        CHECK(record.entry.kind == 2 && record.entry.size == 0 && record.has_entrycount && record.entrycount == 0);
        e.kind = 1;
    }
    cfg.pack_invalid = false; SBAttrSpec spec = sb_attr_spec(&cfg, false);
    size_t n = wire_encode(buffer, spec, e, "bad", SB_A_RETURNED | SB_A_ERROR, 0, 0, EACCES, 0, 0);
    CHECK(sb_attr_decode(buffer, n, spec, SB_LOGICAL, &record) && record.error == EACCES && !record.name);
    n = wire_encode(buffer, spec, e, "missing", spec.common, 0, 0, 0, 0, 0);
    CHECK(sb_attr_decode(buffer, n, spec, SB_LOGICAL, &record)); CHECK(!(record.entry.valid & SB_VALID_SIZE));
    n = wire_encode(buffer, spec, e, "not/a/component", spec.common, 0, spec.file, 0, 0, 0);
    CHECK(!sb_attr_decode(buffer, n, spec, SB_LOGICAL, &record));
    e.size = UINT64_MAX;
    n = wire_encode(buffer, spec, e, "negative", spec.common, 0, spec.file, 0, 0, 0);
    CHECK(!sb_attr_decode(buffer, n, spec, SB_LOGICAL, &record));
    puts("unit: synthetic attribute layouts and invalid records passed");
}
static void collector_tests(void) {
    SBConfig cfg = config(); const uint64_t high = UINT64_C(1) << 48;
    SBCollector c[2]; sb_collector_init(c, true); sb_collector_init(c + 1, true);
    add(c, &cfg, entry(high + 1, 1, 2, 0), "dir");
    add(c + 1, &cfg, entry(high + 2, high + 1, 1, 11), "file");
    add(c, &cfg, entry(high + 2, 1, 1, 11), "hardlink");
    add(c + 1, &cfg, entry(high + 3, 1, 5, 999), "symlink");
    SBResult r = {.root_device = 42, .root_id = 1}; SBInventory inventory;
    sb_collect_finalize(c, 2, &cfg, &r, &inventory);
    CHECK(r.completion == SB_COMPLETE && r.graph_valid && r.entries == 4);
    CHECK(r.files == 2 && r.unique_files == 1 && r.bytes == 22 && r.unique_bytes == 11 && r.graph_root_bytes == 22);
    for (size_t j = 0; j < inventory.count; ++j) if (inventory.entries[j].kind == 2) CHECK(inventory.entries[j].subtree_bytes == 11);
    FILE *f = tmpfile(); CHECK(f); CHECK(sb_manifest_write(fileno(f), &inventory)); rewind(f);
    SBInventory copy; CHECK(sb_manifest_read(fileno(f), &copy, UINT64_C(1) << 28)); fclose(f);
    char reason[256]; CHECK(sb_inventory_equal(&copy, &inventory, reason, sizeof(reason)));
    SBCollector hash; sb_collector_init(&hash, true); cfg.reduce = SB_REDUCE_HASH;
    for (size_t j = inventory.count; j > 0; --j) add(&hash, &cfg, inventory.entries[j - 1], (char *)inventory.names + inventory.entries[j - 1].name_offset);
    SBResult hr = {.root_device = 42, .root_id = 1}; SBInventory hi;
    sb_collect_finalize(&hash, 1, &cfg, &hr, &hi);
    CHECK(hr.digest_a == r.digest_a && hr.digest_b == r.digest_b && hr.digest_c == r.digest_c && hr.unique_bytes == r.unique_bytes);
    CHECK(sb_inventory_equal(&hi, &inventory, reason, sizeof(reason)));
    sb_collector_destroy(&hash); sb_inventory_destroy(&hi);
    sb_inventory_destroy(&copy); sb_inventory_destroy(&inventory);
    sb_collector_destroy(c); sb_collector_destroy(c + 1);
    sb_collector_init(c, true); r = (SBResult){.root_device = 42, .root_id = 1};
    sb_collect_finalize(c, 1, &cfg, &r, &inventory); CHECK(r.entries == 0 && r.graph_valid);
    sb_collector_destroy(c); sb_inventory_destroy(&inventory);
    sb_collector_init(c, true);
    add(c, &cfg, entry(2, 3, 2, 0), "a"); add(c, &cfg, entry(3, 2, 2, 0), "b");
    r = (SBResult){.root_device = 42, .root_id = 1}; sb_collect_finalize(c, 1, &cfg, &r, &inventory);
    CHECK(r.completion == SB_PARTIAL && !r.graph_valid); sb_collector_destroy(c); sb_inventory_destroy(&inventory);
    sb_collector_init(c, true); SBEntry unknown = entry(2, 1, 1, 123); unknown.valid &= ~SB_VALID_SIZE;
    add(c, &cfg, unknown, "unknown"); r = (SBResult){.root_device = 42, .root_id = 1};
    sb_collect_finalize(c, 1, &cfg, &r, &inventory); CHECK(r.unknown_sizes == 1 && r.completion == SB_PARTIAL);
    sb_collector_destroy(c); sb_inventory_destroy(&inventory);
    puts("unit: hard-link identity, graph totals, cycles, unknown sizes, empty inventories, reductions and manifest roundtrip passed");
}
int main(void) {
    captured_parser_tests(); parser_edge_tests(); captured_mutation_tests(); parser_tests(); collector_tests();
    uint64_t x = UINT64_MAX; CHECK(!sb_add_u64(&x, 1) && x == UINT64_MAX);
    puts("unit: all tests passed"); return 0;
}
