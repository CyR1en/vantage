#include "scan.h"
#include <errno.h>
#include <string.h>

int vs_execute(const VSConfig *cfg, VSResult *r, VSInventory *inventory) {
    memset(r, 0, sizeof(*r));
    memset(inventory, 0, sizeof(*inventory));
    r->completion = VS_COMPLETE;
    vs_mem_limit(cfg->memory_limit);
    atomic_store_explicit(&vs_progress_entries, 0, memory_order_relaxed);
    uint64_t start = vs_now_ns();
#ifndef __APPLE__
    if (cfg->method == VS_BULK) {
        vs_failure(r, VS_UNSUPPORTED, ENOTSUP, "bulk scanning requires macOS");
    }
    if (cfg->size == VS_ALLOCATED) {
        vs_failure(r, VS_UNSUPPORTED, ENOTSUP,
                   "canonical ATTR_FILE_ALLOCSIZE accounting requires macOS");
    }
#endif
    if (r->completion == VS_COMPLETE) {
        vs_walk_run(cfg, r, inventory);
    }
    r->total_ns = vs_now_ns() - start;
    return r->completion == VS_COMPLETE ? 0 : r->completion == VS_UNSUPPORTED ? 2 : 1;
}
