#include "scanbench.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

/* An explicitly separate collector-only microbenchmark. Loading is untimed;
 * allocation, copying, digest, unique reduction, and tree building are timed. */
int sb_replay(const char *manifest, SBConfig *cfg, unsigned rounds, FILE *out) {
    int fd = open(manifest, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "scanbench: replay: %s\n", strerror(errno)); return 1; }
    sb_mem_limit(cfg->memory_limit);
    SBInventory source = {0};
    bool read_ok = sb_manifest_read(fd, &source, cfg->memory_limit);
    if (read_ok) {
        unsigned char extra; ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n != 0) read_ok = false;
    }
    close(fd);
    if (!read_ok) { sb_inventory_destroy(&source); fprintf(stderr, "scanbench: invalid or oversized inventory\n"); return 1; }
    cfg->task = source.task; cfg->size = source.size;
    cfg->keep_manifest = true;
    int result = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        SBCollector collector; SBInventory output = {0}; SBResult r = {0};
        r.root_device = source.root_device; r.root_id = source.root_id;
        uint64_t start = sb_now_ns();
        sb_collector_init(&collector, true);
        for (size_t j = 0; j < source.count; ++j)
            sb_collect(&collector, cfg, source.entries[j], source.names + source.entries[j].name_offset);
        uint64_t collected = sb_now_ns();
        sb_collect_finalize(&collector, 1, cfg, &r, &output);
        uint64_t end = sb_now_ns();
        char reason[256];
        bool equal = sb_inventory_equal(&source, &output, reason, sizeof(reason));
        fprintf(out, "{\"schema\":1,\"version\":\"%s\",\"event\":\"memory-replay\",\"method\":\"collector-replay\",\"cache\":\"memory-replay\",\"source\":", SB_VERSION);
        sb_json_string(out, manifest);
        fprintf(out, ",\"round\":%u,\"entries\":%zu,\"total_ns\":%" PRIu64 ",\"collect_ns\":%" PRIu64 ",\"finalize_ns\":%" PRIu64 ",\"managed_peak_bytes\":%" PRIu64,
            round, source.count, end - start, collected - start, end - collected, sb_mem_peak());
        fputs(",\"task\":", out); sb_json_string(out, sb_task_name(cfg->task));
        fputs(",\"reduction\":", out); sb_json_string(out, cfg->reduce == SB_REDUCE_SORT ? "sort" : "hash");
        fprintf(out, ",\"verification\":\"%s\",\"completion\":\"%s\",\"filesystem_io_timed\":false,\"darwin_record_parser_timed\":false,\"fresh_process_per_trial\":false,\"reason\":",
            equal ? "verified" : "mismatch", sb_completion_name(r.completion));
        sb_json_string(out, equal ? r.reason : reason); fputs("}\n", out);
        if (!equal || r.completion != SB_COMPLETE) result = 1;
        sb_collector_destroy(&collector); sb_inventory_destroy(&output);
    }
    sb_inventory_destroy(&source); return result;
}
