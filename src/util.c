#include "scan.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* max_align_t ensures every pointer returned by this allocator is fully aligned. */
typedef union {
    struct {
        size_t size;
    } h;

    max_align_t alignment;
} MemHeader;

static atomic_uint_fast64_t memory_used;
static uint64_t memory_cap = UINT64_MAX;
atomic_uint_fast64_t vs_progress_entries;

_Noreturn void vs_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("vantage: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(71);
}

void vs_mem_limit(uint64_t bytes) {
    memory_cap = bytes ? bytes : UINT64_MAX;
}

static void reserve_memory(size_t n) {
    uint64_t old = atomic_fetch_add(&memory_used, n);
    if (n > memory_cap || old > memory_cap - n) {
        vs_die("managed memory limit exceeded");
    }
}

void *vs_alloc(size_t n) {
    if (!n) {
        n = 1;
    }
    if (n > SIZE_MAX - sizeof(MemHeader)) {
        vs_die("allocation size overflow");
    }
    reserve_memory(n);
    MemHeader *h = malloc(sizeof(*h) + n);
    if (!h) {
        vs_die("out of memory allocating %zu bytes", n);
    }
    h->h.size = n;
    return h + 1;
}

void *vs_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        vs_die("allocation multiplication overflow");
    }
    size_t bytes = n * size;
    void *p = vs_alloc(bytes);
    memset(p, 0, bytes);
    return p;
}

void *vs_realloc(void *p, size_t n) {
    if (!p) {
        return vs_alloc(n);
    }
    if (!n) {
        n = 1;
    }
    if (n > SIZE_MAX - sizeof(MemHeader)) {
        vs_die("allocation size overflow");
    }
    MemHeader *old = (MemHeader *)p - 1;
    size_t was = old->h.size;
    if (n > was) {
        reserve_memory(n - was);
    }
    MemHeader *h = realloc(old, sizeof(*h) + n);
    if (!h) {
        vs_die("out of memory reallocating %zu bytes", n);
    }
    if (n < was) {
        atomic_fetch_sub(&memory_used, was - n);
    }
    h->h.size = n;
    return h + 1;
}

void vs_free(void *p) {
    if (!p) {
        return;
    }
    MemHeader *h = (MemHeader *)p - 1;
    atomic_fetch_sub(&memory_used, h->h.size);
    free(h);
}

uint64_t vs_now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        vs_die("monotonic clock failed");
    }
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}

uint64_t vs_mix(uint64_t x) {
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

bool vs_add_u64(uint64_t *a, uint64_t b) {
    if (b > UINT64_MAX - *a) {
        return false;
    }
    *a += b;
    return true;
}

uint32_t vs_kind(mode_t m) {
    /* Values intentionally match Darwin enum vtype, not DT_* or S_IF*. */
    if (S_ISREG(m)) {
        return 1;
    }
    if (S_ISDIR(m)) {
        return 2;
    }
    if (S_ISBLK(m)) {
        return 3;
    }
    if (S_ISCHR(m)) {
        return 4;
    }
    if (S_ISLNK(m)) {
        return 5;
    }
    if (S_ISSOCK(m)) {
        return 6;
    }
    if (S_ISFIFO(m)) {
        return 7;
    }
    return 0;
}

const char *vs_method_name(VSMethod m) {
    return m == VS_POSIX ? "posix" : m == VS_BULK ? "bulk" : "invalid";
}

const char *vs_size_name(VSSize s) {
    return s == VS_LOGICAL ? "logical" : "allocated";
}

const char *vs_completion_name(VSCompletion c) {
    const char *v[] = {"complete", "partial", "unsupported", "failed"};
    return v[c];
}

void vs_failure(VSResult *r, VSCompletion c, int err, const char *fmt, ...) {
    if (c > r->completion || !r->reason[0]) {
        r->completion = c;
        r->error_code = err;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(r->reason, sizeof(r->reason), fmt, ap);
        va_end(ap);
    }
}

void vs_counter_merge(VSCounters *to, const VSCounters *from) {
#define ADD(f) to->f += from->f
    ADD(enrichment_calls);
    ADD(excluded_boundaries);
    ADD(errors);
    ADD(permission_errors);
    ADD(identity_races);
    ADD(malformed_records);
    ADD(duplicates);
    ADD(missing_parents);
    ADD(unknown_attributes);
#undef ADD
}
