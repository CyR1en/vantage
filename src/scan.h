#ifndef VANTAGE_SCAN_H
#define VANTAGE_SCAN_H
/* Vantage scanner: MIT licensed; see LICENSE. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#define VS_PATH_CAP 16384
#define VS_MAX_WORKERS 64
#define VS_NO_INDEX UINT64_MAX
#define VS_VALID_SIZE 1u
#define VS_VALID_ID 2u
#define VS_VALID_PARENT 4u
#define VS_VALID_KIND 8u
#define VS_VALID_DEVICE 16u
#define VS_REQUIRED (VS_VALID_ID | VS_VALID_PARENT | VS_VALID_KIND | VS_VALID_DEVICE)

typedef enum { VS_POSIX, VS_BULK } VSMethod;

typedef enum { VS_LOGICAL, VS_ALLOCATED } VSSize;

typedef enum { VS_COMPLETE, VS_PARTIAL, VS_UNSUPPORTED, VS_FAILED } VSCompletion;

typedef struct {
    char root[VS_PATH_CAP];
    VSMethod method;
    VSSize size;
    uint32_t workers, queue_limit, fd_limit;
    uint64_t buffer_size, memory_limit, timeout_ns;
} VSConfig;

typedef struct {
    uint64_t device, object_id, parent_id, name_offset, size;
    uint64_t parent_index, subtree_bytes, subtree_unknown;
    uint32_t name_length, kind, valid;
} VSEntry;

typedef struct {
    VSEntry *entries;
    size_t count, capacity;
    unsigned char *names;
    size_t names_size, names_capacity;
    uint64_t files, directories, bytes, unknown_sizes;
} VSCollector;

typedef struct {
    uint64_t enrichment_calls;
    uint64_t excluded_boundaries, errors, permission_errors;
    uint64_t identity_races, malformed_records, duplicates, missing_parents;
    uint64_t unknown_attributes;
} VSCounters;

typedef struct {
    uint64_t total_ns, scan_ns;
    uint64_t root_device, root_id, entries, files, directories;
    uint64_t unique_objects, unique_files, bytes, unique_bytes, unknown_sizes, graph_root_bytes;
    uint32_t actual_workers;
    VSCompletion completion;
    int error_code;
    bool graph_valid;
    char reason[256];
    VSCounters counters;
} VSResult;

typedef struct {
    VSEntry *entries;
    size_t count;
    unsigned char *names;
    size_t names_size;
    uint64_t root_device, root_id;
} VSInventory;

typedef struct VSPath {
    atomic_uint refs;
    struct VSPath *parent;
    uint64_t device, id;
    size_t depth;
    char name[];
} VSPath;

typedef struct {
    VSPath *path;
    int fd;
    bool reserved_fd;
} VSDirTask;

typedef struct {
    uint64_t device, id;
    bool occupied;
} VSSeenSlot;

struct VSWalk;

typedef struct {
    struct VSWalk *walk;
    VSCollector collector;
    VSCounters counters;
    unsigned char *buffer;
} VSWorker;

typedef struct VSWalk {
    const VSConfig *config;
    VSResult *result;
    int root_fd;
    VSPath *root_path;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    VSDirTask *queue;
    size_t queue_count, queue_capacity;
    uint32_t active, worker_count;
    VSSeenSlot *seen;
    size_t seen_count, seen_capacity;
    uint64_t *mount_ids;
    size_t mount_count;
    atomic_uint queued_fds;
    uint32_t queued_fd_budget;
    atomic_bool stop;
    VSWorker *workers;
    uint64_t deadline;
} VSWalk;

/* Entries enumerated by the running walk; readable by progress observers. */
extern atomic_uint_fast64_t vs_progress_entries;
_Noreturn void vs_die(const char *fmt, ...);
void vs_mem_limit(uint64_t bytes);
void *vs_alloc(size_t n);
void *vs_calloc(size_t n, size_t size);
void *vs_realloc(void *p, size_t n);
void vs_free(void *p);
uint64_t vs_now_ns(void);
uint64_t vs_mix(uint64_t n);
bool vs_add_u64(uint64_t *a, uint64_t b);
uint32_t vs_kind(mode_t mode);
const char *vs_method_name(VSMethod m);
const char *vs_size_name(VSSize s);
const char *vs_completion_name(VSCompletion c);
void vs_failure(VSResult *r, VSCompletion c, int err, const char *fmt, ...);
void vs_counter_merge(VSCounters *to, const VSCounters *from);

void vs_collector_init(VSCollector *c);
void vs_collector_destroy(VSCollector *c);
void vs_collect(VSCollector *c, VSEntry e, const void *name);
void vs_collect_finalize(VSCollector *cs, size_t n, VSResult *r, VSInventory *inventory);
void vs_inventory_destroy(VSInventory *i);

void vs_walk_run(const VSConfig *c, VSResult *r, VSInventory *i);
void vs_posix_directory(VSWorker *w, VSDirTask *task);
void vs_bulk_directory(VSWorker *w, VSDirTask *task);
void vs_walk_entry(VSWorker *w, VSDirTask *parent, int parent_fd, VSEntry e, const char *name);
void vs_worker_error(VSWorker *w, int err);
void vs_walk_stop(VSWalk *w, int err, const char *reason);
bool vs_walk_expired(VSWalk *w);

int vs_execute(const VSConfig *c, VSResult *r, VSInventory *inventory);
void vs_json_string(FILE *f, const char *s);
void vs_json_hex(FILE *f, const unsigned char *s, size_t n);
#endif
