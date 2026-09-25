#include "scanbench.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>
#ifdef __APPLE__
#include <sys/mount.h>
#include <sys/sysctl.h>
#else
#include <sys/vfs.h>
#endif
#ifndef SB_BUILD_FLAGS
#define SB_BUILD_FLAGS "unrecorded"
#endif
#ifndef SB_SDK
#define SB_SDK "not-recorded"
#endif
#ifndef SB_REVISION
#define SB_REVISION "source-archive"
#endif
/* max_align_t ensures every pointer returned by this allocator is fully aligned. */
typedef union { struct { size_t size; } h; max_align_t alignment; } MemHeader;
static atomic_uint_fast64_t memory_used, memory_peak;
static uint64_t memory_cap = UINT64_MAX;
atomic_uint_fast64_t sb_progress_entries;

_Noreturn void sb_die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("scanbench: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(71);
}
void sb_mem_limit(uint64_t bytes) { memory_cap = bytes ? bytes : UINT64_MAX; }
uint64_t sb_mem_peak(void) { return atomic_load(&memory_peak); }
static void reserve_memory(size_t n) {
    uint64_t old = atomic_fetch_add(&memory_used, n);
    if (n > memory_cap || old > memory_cap - n) sb_die("managed memory limit exceeded");
    uint64_t peak = atomic_load(&memory_peak), now = old + n;
    while (peak < now && !atomic_compare_exchange_weak(&memory_peak, &peak, now)) { }
}
void *sb_alloc(size_t n) {
    if (!n) n = 1;
    if (n > SIZE_MAX - sizeof(MemHeader)) sb_die("allocation size overflow");
    reserve_memory(n);
    MemHeader *h = malloc(sizeof(*h) + n);
    if (!h) sb_die("out of memory allocating %zu bytes", n);
    h->h.size = n; return h + 1;
}
void *sb_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) sb_die("allocation multiplication overflow");
    size_t bytes = n * size;
    void *p = sb_alloc(bytes); memset(p, 0, bytes); return p;
}
void *sb_realloc(void *p, size_t n) {
    if (!p) return sb_alloc(n);
    if (!n) n = 1;
    if (n > SIZE_MAX - sizeof(MemHeader)) sb_die("allocation size overflow");
    MemHeader *old = (MemHeader *)p - 1;
    size_t was = old->h.size;
    if (n > was) reserve_memory(n - was);
    MemHeader *h = realloc(old, sizeof(*h) + n);
    if (!h) sb_die("out of memory reallocating %zu bytes", n);
    if (n < was) atomic_fetch_sub(&memory_used, was - n);
    h->h.size = n; return h + 1;
}
void sb_free(void *p) {
    if (!p) return;
    MemHeader *h = (MemHeader *)p - 1;
    atomic_fetch_sub(&memory_used, h->h.size); free(h);
}
uint64_t sb_now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) sb_die("monotonic clock failed");
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
uint64_t sb_realtime_ns(void) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
uint64_t sb_mix(uint64_t x) {
    x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb); return x ^ (x >> 31);
}
uint64_t sb_random(uint64_t *state) {
    *state += UINT64_C(0x9e3779b97f4a7c15); return sb_mix(*state);
}
uint64_t sb_hash_bytes(const void *v, size_t n, uint64_t seed) {
    const unsigned char *p = v;
    uint64_t h = UINT64_C(14695981039346656037) ^ seed;
    for (size_t j = 0; j < n; ++j) { h ^= p[j]; h *= UINT64_C(1099511628211); }
    return sb_mix(h);
}
bool sb_add_u64(uint64_t *a, uint64_t b) {
    if (b > UINT64_MAX - *a) return false;
    *a += b; return true;
}
bool sb_write_all(int fd, const void *data, size_t n) {
    const unsigned char *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p += (size_t)k; n -= (size_t)k;
    }
    return true;
}
bool sb_read_all(int fd, void *data, size_t n) {
    unsigned char *p = data;
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return false;
        p += (size_t)k; n -= (size_t)k;
    }
    return true;
}
uint32_t sb_kind(mode_t m) {
    /* Values intentionally match Darwin enum vtype, not DT_* or S_IF*. */
    if (S_ISREG(m)) return 1;
    if (S_ISDIR(m)) return 2;
    if (S_ISBLK(m)) return 3;
    if (S_ISCHR(m)) return 4;
    if (S_ISLNK(m)) return 5;
    if (S_ISSOCK(m)) return 6;
    if (S_ISFIFO(m)) return 7;
    return 0;
}
const char *sb_method_name(SBMethod m) {
    static const char *v[] = {"posix", "posix-par", "bulk", "bulk-par", "catalog", "catalog-pipe", "catalog-partition"};
    return (unsigned)m < SB_METHOD_COUNT ? v[m] : "invalid";
}
const char *sb_task_name(SBTask t) { return t == SB_ENUMERATE ? "enumerate" : t == SB_USAGE ? "usage" : "tree"; }
const char *sb_size_name(SBSize s) { return s == SB_LOGICAL ? "logical" : "allocated"; }
const char *sb_order_name(SBOrder o) { const char *v[] = {"dfs","fifo","random","id-band"}; return v[o]; }
const char *sb_completion_name(SBCompletion c) { const char *v[] = {"complete","partial","unsupported","failed"}; return v[c]; }
const char *sb_verification_name(SBVerification v) { const char *a[] = {"unverified","verified","mismatch"}; return a[v]; }
const char *sb_cache_name(SBCache c) { const char *v[] = {"warm","first-pass","reboot-prepared","remounted-fixture"}; return v[c]; }
const char *sb_consistency_name(SBConsistency c) { const char *v[] = {"live-best-effort","quiescent","immutable"}; return v[c]; }
void sb_config_key(const SBConfig *c, char *s, size_t n) {
    snprintf(s, n, "%s:w%u:b%" PRIu64 ":%s:f%u:s%u:pack%d:empty%d:adapt%d:ec%d:lc%d:uuid%d:notype%d:reduce%d:part%u:fd%u:q%u:diag%d",
        sb_method_name(c->method), c->workers, c->buffer_size, sb_order_name(c->order),
        c->frontier, c->id_shift, c->pack_invalid, c->skip_empty, c->adaptive,
        c->extra_entrycount, c->extra_linkcount, c->extra_uuid, c->omit_objtype,
        c->reduce, c->catalog_partitions, c->fd_limit, c->queue_limit, c->diagnostic);
}
void sb_failure(SBResult *r, SBCompletion c, int err, const char *fmt, ...) {
    if (c > r->completion || !r->reason[0]) {
        r->completion = c; r->error_code = err;
        va_list ap; va_start(ap, fmt); vsnprintf(r->reason, sizeof(r->reason), fmt, ap); va_end(ap);
    }
}
void sb_counter_merge(SBCounters *to, const SBCounters *from) {
#define ADD(f) to->f += from->f
    ADD(readdir_calls); ADD(stat_calls); ADD(bulk_calls); ADD(search_calls); ADD(opens); ADD(reopens);
    ADD(enrichment_calls); ADD(batches); ADD(batch_entries); ADD(metadata_bytes);
    ADD(excluded_boundaries); ADD(empty_opens_avoided); ADD(errors); ADD(permission_errors);
    ADD(vanished); ADD(identity_races); ADD(malformed_records); ADD(duplicates); ADD(missing_parents);
    ADD(unknown_attributes); ADD(catalog_restarts); ADD(adaptive_changes); ADD(search_ns); ADD(traversal_ns);
    if (from->max_batch > to->max_batch) to->max_batch = from->max_batch;
    if (from->queue_peak > to->queue_peak) to->queue_peak = from->queue_peak;
#undef ADD
}
void sb_environment(const SBConfig *c, SBResult *r, int fd) {
    struct utsname u; memset(&u, 0, sizeof(u)); uname(&u);
    snprintf(r->os, sizeof(r->os), "%.60s %.90s", u.sysname, u.release);
    snprintf(r->machine, sizeof(r->machine), "%.79s", u.machine);
    snprintf(r->build, sizeof(r->build), "compiler=%s; flags=%s; source=%s; sdk=%s", __VERSION__, SB_BUILD_FLAGS, SB_REVISION, SB_SDK);
    r->uid = (uint32_t)geteuid(); r->gid = (uint32_t)getegid();
    long cpus = sysconf(_SC_NPROCESSORS_ONLN); r->cpus = cpus > 0 ? (uint32_t)cpus : 0;
    struct stat st = {0};
    if (!fstat(fd, &st)) { r->root_device = (uint64_t)st.st_dev; r->root_id = (uint64_t)st.st_ino; }
#ifdef __APPLE__
    size_t model_size = sizeof(r->hardware_model), version_size = sizeof(r->product_version), memory_size = sizeof(r->physical_memory_bytes);
    if (sysctlbyname("hw.model", r->hardware_model, &model_size, NULL, 0)) r->hardware_model[0] = 0;
    if (sysctlbyname("kern.osproductversion", r->product_version, &version_size, NULL, 0)) r->product_version[0] = 0;
    if (sysctlbyname("hw.memsize", &r->physical_memory_bytes, &memory_size, NULL, 0)) r->physical_memory_bytes = 0;
    r->hardware_model[sizeof(r->hardware_model) - 1] = 0;
    r->product_version[sizeof(r->product_version) - 1] = 0;
    struct statfs f;
    if (!fstatfs(fd, &f)) {
        snprintf(r->filesystem, sizeof(r->filesystem), "%s", f.f_fstypename);
        r->readonly_mount = (f.f_flags & MNT_RDONLY) != 0;
        struct stat root;
        r->volume_root = !stat(f.f_mntonname, &root) && root.st_dev == st.st_dev && root.st_ino == st.st_ino;
    }
#else
    struct statfs f;
    if (!fstatfs(fd, &f)) snprintf(r->filesystem, sizeof(r->filesystem), "linux:0x%lx", (unsigned long)f.f_type);
    int p = openat(fd, "..", 0 | O_DIRECTORY | O_CLOEXEC);
    if (p >= 0) { struct stat ps; if (!fstat(p, &ps)) r->volume_root = ps.st_dev != st.st_dev || ps.st_ino == st.st_ino; close(p); }
#endif
    snprintf(r->scope_note, sizeof(r->scope_note), "one device; boundary entries excluded; no symlink traversal; consistency is caller-asserted");
    r->environment_id = sb_hash_bytes(r->os, strlen(r->os), 0);
    r->environment_id ^= sb_hash_bytes(r->machine, strlen(r->machine), 1);
    r->environment_id ^= sb_hash_bytes(r->build, strlen(r->build), 2);
    r->environment_id ^= sb_hash_bytes(r->hardware_model, strlen(r->hardware_model), 4);
    r->environment_id ^= sb_hash_bytes(r->product_version, strlen(r->product_version), 5);
    r->environment_id ^= sb_mix(r->physical_memory_bytes);
    r->environment_id ^= sb_mix(((uint64_t)r->uid << 32) | r->cpus);
    r->group_id = r->environment_id ^ sb_hash_bytes(c->root, strlen(c->root), 3);
    r->group_id ^= sb_mix(r->root_device) ^ sb_mix(r->root_id);
    r->group_id ^= sb_mix((uint64_t)c->task | ((uint64_t)c->size << 8) | ((uint64_t)c->cache << 16) | ((uint64_t)c->consistency << 24));
}
