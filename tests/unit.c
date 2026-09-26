#include "scan.h"
#include "attrs.h"
#include "wire.h"
#include "fixtures/attrs/captured.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static VSAttrSpec fixture_spec(bool uuid, bool omit, bool pack) {
    VSAttrSpec spec = vs_attr_spec(VS_LOGICAL);
    spec.pack_invalid = pack;
    if (uuid) {
        spec.common |= VS_A_UUID;
    }
    if (omit) {
        spec.common &= ~VS_A_OBJTYPE;
    }
    return spec;
}

static uint64_t random_mutation(uint64_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static VSEntry entry(uint64_t id, uint64_t parent, unsigned kind, uint64_t size) {
    return (VSEntry){.device = 42,
                     .object_id = id,
                     .parent_id = parent,
                     .kind = kind,
                     .size = size,
                     .valid = VS_REQUIRED | VS_VALID_SIZE};
}

static void add(VSCollector *c, VSEntry e, const char *name) {
    e.name_length = (uint32_t)strlen(name);
    vs_collect(c, e, name);
}

static void captured_parser_tests(void) {
    const char *names[] = {"symlink", "file", "dir", "fifo"};
    const uint32_t kinds[] = {5, 1, 2, 7};
    const uint64_t ids[] = {111222590, 111222588, 111222589, 111222591};
    const uint64_t logical[] = {4, 7, 0, 0};
    unsigned total = 0;
    for (size_t i = 0; i < sizeof(captured_attrs) / sizeof(*captured_attrs); ++i) {
        const CapturedAttrs *f = captured_attrs + i;
        VSAttrSpec spec = fixture_spec(f->uuid, f->omit, f->pack);
        size_t offset = 0, count = f->denied ? 2 : 4;
        for (size_t j = 0; j < count; ++j) {
            VSAttrRecord r;
            bool decoded =
                vs_attr_decode(f->bytes + offset, f->length - offset, spec, VS_LOGICAL, &r);
            if (!decoded) {
                fprintf(stderr, "captured record rejected: %s[%zu]\n", f->name, j);
            }
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
                CHECK((r.entry.valid & (VS_VALID_ID | VS_VALID_DEVICE)) ==
                      (VS_VALID_ID | VS_VALID_DEVICE));
                CHECK(r.entry.object_id == id && r.entry.device == 16777230);
                CHECK((r.entry.valid & VS_VALID_KIND) == (f->omit ? 0 : VS_VALID_KIND));
                if (!f->omit) {
                    CHECK(r.entry.kind == kind);
                }
                CHECK(r.has_mountstatus == (kind == 2) && r.has_logical == (kind != 2));
                if (kind != 2) {
                    CHECK(r.logical == (f->denied ? 7 : logical[j]));
                }
                if (!f->omit) {
                    CHECK(r.entry.size == (kind == 1 ? 7 : 0));
                }
            }
            CHECK(r.record_size && r.record_size <= f->length - offset);
            for (size_t n = 0; n < r.record_size; ++n) {
                VSAttrRecord truncated;
                CHECK(!vs_attr_decode(f->bytes + offset, n, spec, VS_LOGICAL, &truncated));
            }
            offset += r.record_size;
            ++total;
        }
        CHECK(offset == f->length);
    }
    CHECK(total == 48);
    puts("unit: 48 independently captured macOS attribute records and all truncations passed");
}

static void put32(unsigned char *bytes, size_t offset, uint32_t value) {
    memcpy(bytes + offset, &value, 4);
}

static uint32_t get32(const unsigned char *bytes, size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, 4);
    return value;
}

static void parser_edge_tests(void) {
    VSAttrSpec spec = fixture_spec(false, false, true);
    unsigned char b[512];
    VSAttrRecord r;
    const unsigned char *captured = normal_uuid0_omit0_pack1;
    size_t n = get32(captured, 0);
    uint32_t common = get32(captured, 4);
    /* Both type masks, or a type that contradicts its returned mask. */
    memcpy(b, captured, n);
    put32(b, 12, VS_D_MOUNTSTATUS);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    put32(b, 16, 0);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    memcpy(b, captured, n);
    put32(b, 40, 2);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    put32(b, 40, 0);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    put32(b, 40, 10);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    /* A poisoned invalid type is never promoted into a valid type. */
    memcpy(b, captured, n);
    put32(b, 4, common & ~VS_A_OBJTYPE);
    put32(b, 40, UINT32_MAX);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    CHECK(!(r.entry.valid & VS_VALID_KIND) && !r.entry.kind && r.has_logical && r.logical == 4);
    /* No valid type-specific values: ignore the bounded tail and require enrichment. */
    put32(b, 16, 0);
    memset(b + 56, 0xff, 8);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    CHECK(!(r.entry.valid & (VS_VALID_KIND | VS_VALID_SIZE)) && !r.has_logical && !r.logical);
    put32(b, 4, common & ~(VS_A_OBJTYPE | VS_A_FLAGS | VS_A_FILEID | VS_A_DEVID));
    memset(b + 36, 0xff, 20);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    CHECK(!r.entry.valid && !r.entry.device && !r.entry.object_id && !r.flags && !r.has_flags);
    /* Names must lie after the parsed common prefix, even for an opaque tail. */
    put32(b, 28, 16);
    put32(b, 32, 4);
    memcpy(b + 44, "abc", 4);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    /* Returned ERROR governs its physical presence even when packing is enabled. */
    VSEntry e = entry(21, 8, 1, 19);
    n = wire_encode(b, spec, e, "no-error", spec.common & ~VS_A_ERROR, 0, spec.file, 0, 0);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r) && r.entry.size == 19 && !r.error);
    /* Invalid packed sizes must not become values or signed-length failures. */
    e.size = UINT64_MAX;
    n = wire_encode(b, spec, e, "invalid-size", spec.common, 0, 0, 0, 0);
    memset(b + 28 + get32(b, 28) - 8, 0xff, 8);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    CHECK(!(r.entry.valid & VS_VALID_SIZE) && !r.has_logical && !r.logical);
    /* A common-only record has no file section to infer. */
    spec = fixture_spec(false, true, true);
    spec.file = 0;
    n = wire_encode(b, spec, e, "enumerated", spec.common, 0, 0, 0, 0);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r) && !strcmp(r.name, "enumerated"));
    CHECK(!(r.entry.valid & (VS_VALID_KIND | VS_VALID_SIZE)));
    /* Real EACCES records have zero type masks but retain a bounded name reference. */
    spec = fixture_spec(true, false, true);
    captured = error_uuid1_omit0_pack1;
    n = get32(captured, 0);
    memcpy(b, captured, n);
    CHECK(vs_attr_decode(b, n, spec, VS_LOGICAL, &r) && r.error == EACCES && !r.entry.valid);
    for (unsigned trial = 0; trial < 6; ++trial) {
        memcpy(b, captured, n);
        size_t name = 28 + get32(b, 28);
        uint32_t length = get32(b, 32);
        if (trial == 0) {
            put32(b, 28, UINT32_MAX); /* Backward overlap. */
        }
        if (trial == 1) {
            put32(b, 28, INT32_MAX); /* Beyond the record. */
        }
        if (trial == 2) {
            put32(b, 32, 0);
        }
        if (trial == 3) {
            b[name + length - 1] = 'x';
        }
        if (trial == 4) {
            b[name + 1] = '\0';
        }
        if (trial == 5) {
            b[name + 1] = '/';
        }
        CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r));
    }
    memcpy(b, captured, n);
    put32(b, 24, 0);
    put32(b, 4, VS_A_RETURNED | VS_A_ERROR);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r)); /* Success requires a name. */
    memcpy(b, captured, n);
    const uint32_t unrequested_common = UINT32_C(0x04000000);
    put32(b, 4, get32(b, 4) | unrequested_common);
    CHECK(!vs_attr_decode(b, n, spec, VS_LOGICAL, &r)); /* Unrequested returned attribute. */
    puts(
        "unit: packed validity, ambiguous type, contradictory masks and bounded error names passed");
}

static void captured_mutation_tests(void) {
    uint64_t seed = 5;
    unsigned char corrupt[512];
    for (unsigned trial = 0; trial < 100000; ++trial) {
        const CapturedAttrs *f =
            captured_attrs + trial % (sizeof(captured_attrs) / sizeof(*captured_attrs));
        VSAttrSpec spec = fixture_spec(f->uuid, f->omit, f->pack);
        size_t n = get32(f->bytes, 0);
        CHECK(n <= sizeof(corrupt));
        memcpy(corrupt, f->bytes, n);
        for (unsigned k = 0; k < 3; ++k) {
            corrupt[random_mutation(&seed) % n] ^= (unsigned char)random_mutation(&seed);
        }
        VSAttrRecord r;
        (void)vs_attr_decode(corrupt, n, spec, VS_LOGICAL, &r);
    }
    puts(
        "unit: 100000 mutations across captured packed/unpacked, omitted-type and error records passed");
}

static void parser_tests(void) {
    VSEntry e = entry(UINT64_C(0xf000000000000021), UINT64_C(0xe000000000000008), 1, 19);
    unsigned char buffer[512], corrupt[512];
    VSAttrRecord record;
    for (unsigned pack = 0; pack < 2; ++pack) {
        VSAttrSpec spec = fixture_spec(false, false, pack != 0);
        size_t length = wire_encode(buffer, spec, e, "alpha", spec.common, 0, spec.file, 0, 0);
        CHECK(vs_attr_decode(buffer, length, spec, VS_LOGICAL, &record));
        CHECK(record.entry.object_id == e.object_id && record.entry.size == 19 &&
              !strcmp(record.name, "alpha"));
        CHECK(record.record_size == length);
        for (size_t n = 0; n < length; ++n) {
            CHECK(!vs_attr_decode(buffer, n, spec, VS_LOGICAL, &record));
        }
        memcpy(corrupt, buffer, length);
        corrupt[0] = 3;
        CHECK(!vs_attr_decode(corrupt, length, spec, VS_LOGICAL, &record));
        e.kind = 2;
        length = wire_encode(buffer, spec, e, "directory", spec.common, spec.dir, 0, 0, 0);
        CHECK(vs_attr_decode(buffer, length, spec, VS_LOGICAL, &record));
        CHECK(record.entry.kind == 2 && record.entry.size == 0 && record.has_mountstatus &&
              record.mountstatus == 0);
        e.kind = 1;
    }
    VSAttrSpec spec = fixture_spec(false, false, false);
    size_t n = wire_encode(buffer, spec, e, "bad", VS_A_RETURNED | VS_A_ERROR, 0, 0, EACCES, 0);
    CHECK(vs_attr_decode(buffer, n, spec, VS_LOGICAL, &record) && record.error == EACCES &&
          !record.name);
    n = wire_encode(buffer, spec, e, "missing", spec.common, 0, 0, 0, 0);
    CHECK(vs_attr_decode(buffer, n, spec, VS_LOGICAL, &record));
    CHECK(!(record.entry.valid & VS_VALID_SIZE));
    n = wire_encode(buffer, spec, e, "not/a/component", spec.common, 0, spec.file, 0, 0);
    CHECK(!vs_attr_decode(buffer, n, spec, VS_LOGICAL, &record));
    e.size = UINT64_MAX;
    n = wire_encode(buffer, spec, e, "negative", spec.common, 0, spec.file, 0, 0);
    CHECK(!vs_attr_decode(buffer, n, spec, VS_LOGICAL, &record));
    puts("unit: synthetic attribute layouts and invalid records passed");
}

static void collector_tests(void) {
    const uint64_t high = UINT64_C(1) << 48;
    VSCollector c[2];
    vs_collector_init(c);
    vs_collector_init(c + 1);
    add(c, entry(high + 1, 1, 2, 0), "dir");
    add(c + 1, entry(high + 2, high + 1, 1, 11), "file");
    add(c, entry(high + 2, 1, 1, 11), "hardlink");
    add(c + 1, entry(high + 3, 1, 5, 999), "symlink");
    VSResult r = {.root_device = 42, .root_id = 1};
    VSInventory inventory;
    vs_collect_finalize(c, 2, &r, &inventory);
    CHECK(r.completion == VS_COMPLETE && r.graph_valid && r.entries == 4);
    CHECK(r.files == 2 && r.unique_files == 1 && r.bytes == 22 && r.unique_bytes == 11 &&
          r.graph_root_bytes == 22);
    for (size_t j = 0; j < inventory.count; ++j) {
        if (inventory.entries[j].kind == 2) {
            CHECK(inventory.entries[j].subtree_bytes == 11);
        }
    }
    vs_inventory_destroy(&inventory);
    vs_collector_destroy(c);
    vs_collector_destroy(c + 1);
    vs_collector_init(c);
    r = (VSResult){.root_device = 42, .root_id = 1};
    vs_collect_finalize(c, 1, &r, &inventory);
    CHECK(r.entries == 0 && r.graph_valid);
    vs_collector_destroy(c);
    vs_inventory_destroy(&inventory);
    vs_collector_init(c);
    add(c, entry(2, 3, 2, 0), "a");
    add(c, entry(3, 2, 2, 0), "b");
    r = (VSResult){.root_device = 42, .root_id = 1};
    vs_collect_finalize(c, 1, &r, &inventory);
    CHECK(r.completion == VS_PARTIAL && !r.graph_valid);
    vs_collector_destroy(c);
    vs_inventory_destroy(&inventory);
    vs_collector_init(c);
    VSEntry unknown = entry(2, 1, 1, 123);
    unknown.valid &= ~VS_VALID_SIZE;
    add(c, unknown, "unknown");
    r = (VSResult){.root_device = 42, .root_id = 1};
    vs_collect_finalize(c, 1, &r, &inventory);
    CHECK(r.unknown_sizes == 1 && r.completion == VS_PARTIAL);
    vs_collector_destroy(c);
    vs_inventory_destroy(&inventory);
    puts(
        "unit: hard-link identity, graph totals, cycles, unknown sizes and empty inventories passed");
}

static void identity_conflict_tests(void) {
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        for (unsigned conflict = 0; conflict < 4; ++conflict) {
            VSCollector cs[2];
            vs_collector_init(cs);
            vs_collector_init(cs + 1);
            VSEntry a = entry(UINT64_C(1) << 48, 1, 1, 11), b = a;
            if (conflict == 1) {
                b.size = 12;
            }
            if (conflict == 2) {
                b.valid &= ~VS_VALID_SIZE;
            }
            if (conflict == 3) {
                b.kind = 5;
            }
            add(cs, reverse ? b : a, "file");
            add(cs + 1, reverse ? a : b, "alias");
            VSResult r = {.root_device = 42, .root_id = 1};
            VSInventory inv;
            vs_collect_finalize(cs, 2, &r, &inv);
            CHECK(r.entries == 2 && r.unique_objects == 1 && r.graph_valid);
            CHECK(r.counters.identity_races == (conflict ? 1u : 0u));
            if (conflict) {
                CHECK(r.completion == VS_PARTIAL && r.error_code == ESTALE);
            } else {
                CHECK(r.completion == VS_COMPLETE && r.bytes == 22 && r.unique_bytes == 11);
            }
            vs_inventory_destroy(&inv);
            vs_collector_destroy(cs);
            vs_collector_destroy(cs + 1);
        }
    }
    /* A different device is a distinct identity, and cannot attach to this root. */
    VSCollector c;
    vs_collector_init(&c);
    VSEntry a = entry(UINT64_C(1) << 48, 1, 1, 11), b = a;
    b.device = 43;
    add(&c, a, "file");
    add(&c, b, "other-device");
    VSResult r = {.root_device = 42, .root_id = 1};
    VSInventory inv;
    vs_collect_finalize(&c, 1, &r, &inv);
    CHECK(r.unique_objects == 2 && r.unique_bytes == 22 && !r.counters.identity_races);
    CHECK(r.completion == VS_PARTIAL && !r.graph_valid && r.counters.missing_parents == 1);
    vs_inventory_destroy(&inv);
    vs_collector_destroy(&c);
    puts("unit: device identity and conflicting hard-link metadata across workers passed");
}

int main(void) {
    captured_parser_tests();
    parser_edge_tests();
    captured_mutation_tests();
    parser_tests();
    collector_tests();
    identity_conflict_tests();
    uint64_t x = UINT64_MAX;
    CHECK(!vs_add_u64(&x, 1) && x == UINT64_MAX);
    puts("unit: all tests passed");
    return 0;
}
