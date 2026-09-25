#include "scanbench.h"
#include "attrs.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/attr.h>
#endif

/* A bounded capability probe, not a correctness certificate or warm-cache run. */
int sb_probe(const SBConfig *cfg, FILE *out) {
    SBResult r = {0};
    int fd = open(cfg->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "scanbench: probe: %s\n", strerror(errno)); return 1; }
    sb_environment(cfg, &r, fd);
    int bulk_error = ENOTSUP, catalog_error = ENOTSUP;
    long bulk_records = -1;
    unsigned long catalog_records = 0;
    bool bulk_decoded = false, catalog_attempted = false;
#ifdef __APPLE__
    SBAttrSpec spec = sb_attr_spec(cfg, false);
    struct attrlist attrs = {0}; attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrs.commonattr = spec.common; attrs.dirattr = spec.dir; attrs.fileattr = spec.file;
    size_t capacity = (size_t)cfg->buffer_size;
    unsigned char *buffer = sb_alloc(capacity);
    bulk_records = getattrlistbulk(fd, &attrs, buffer, capacity,
        cfg->pack_invalid ? FSOPT_PACK_INVAL_ATTRS : 0);
    bulk_error = bulk_records < 0 ? errno : 0;
    if (bulk_records > 0) {
        size_t offset = 0; bulk_decoded = true;
        for (long k = 0; k < bulk_records; ++k) {
            SBAttrRecord item;
            if (!sb_attr_decode(buffer + offset, capacity - offset, spec, cfg->size, &item)) { bulk_decoded = false; break; }
            offset += item.record_size;
        }
    }
    if (r.volume_root || cfg->allow_volume_scan) {
        catalog_attempted = true;
        spec = sb_attr_spec(cfg, true); attrs.commonattr = spec.common;
        attrs.dirattr = spec.dir; attrs.fileattr = spec.file;
        struct fssearchblock block; memset(&block, 0, sizeof(block));
        block.returnattrs = &attrs; block.returnbuffer = buffer;
        block.returnbuffersize = capacity; block.maxmatches = 1;
        block.timelimit.tv_usec = 100000;
        block.searchattrs.bitmapcount = ATTR_BIT_MAP_COUNT;
        block.searchattrs.commonattr = ATTR_CMN_NAME;
        unsigned char predicate[16] = {0};
        uint32_t length = 16, name_length = 1; int32_t offset = 8;
        memcpy(predicate, &length, 4); memcpy(predicate + 4, &offset, 4);
        memcpy(predicate + 8, &name_length, 4);
        block.searchparams1 = predicate; block.searchparams2 = predicate;
        block.sizeofsearchparams1 = sizeof(predicate); block.sizeofsearchparams2 = sizeof(predicate);
        struct searchstate state; memset(&state, 0, sizeof(state));
        int rc = searchfs(cfg->root, &block, &catalog_records, 0x08000103u,
            SRCHFS_START | SRCHFS_MATCHFILES | SRCHFS_MATCHDIRS | SRCHFS_MATCHPARTIALNAMES, &state);
        catalog_error = rc < 0 ? errno : 0;
    }
    sb_free(buffer);
#endif
    close(fd);
    fputs("{\"schema\":1,\"event\":\"probe\",\"root\":", out); sb_json_string(out, cfg->root);
    fputs(",\"os\":", out); sb_json_string(out, r.os);
    fputs(",\"machine\":", out); sb_json_string(out, r.machine);
    fputs(",\"filesystem\":", out); sb_json_string(out, r.filesystem);
    fputs(",\"build\":", out); sb_json_string(out, r.build);
    fprintf(out, ",\"uid\":%u,\"gid\":%u,\"cpus\":%u,\"root_device\":\"%" PRIu64 "\",\"root_id\":\"%" PRIu64 "\",\"volume_root\":%s,\"readonly_mount\":%s",
        r.uid, r.gid, r.cpus, r.root_device, r.root_id,
        r.volume_root ? "true" : "false", r.readonly_mount ? "true" : "false");
    fprintf(out, ",\"bulk_first_call\":{\"errno\":%d,\"records\":%ld,\"records_decoded\":%s},\"catalog_first_call\":{\"attempted\":%s,\"errno\":%d,\"records\":%lu}",
        bulk_error, bulk_records, bulk_decoded ? "true" : "false", catalog_attempted ? "true" : "false", catalog_error, catalog_records);
    fputs(",\"full_disk_access\":null,\"note\":\"This probe may warm metadata. Successful first calls do not certify complete coverage, accounting equivalence, or speed. Catalog probing of a subtree requires --allow-volume-scan.\"}\n", out);
    return 0;
}
