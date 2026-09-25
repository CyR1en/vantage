#ifndef SCANBENCH_H
#define SCANBENCH_H
/* scanbench: MIT licensed; see LICENSE. All persistent encodings are versioned. */
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

#define SB_VERSION "0.1.0"
#define SB_SCHEMA 1
#define SB_PATH_CAP 16384
#define SB_MAX_WORKERS 64
#define SB_MAX_CONFIGS 4096
#define SB_NO_INDEX UINT64_MAX
#define SB_VALID_SIZE 1u
#define SB_VALID_ID 2u
#define SB_VALID_PARENT 4u
#define SB_VALID_KIND 8u
#define SB_VALID_DEVICE 16u
#define SB_REQUIRED (SB_VALID_ID | SB_VALID_PARENT | SB_VALID_KIND | SB_VALID_DEVICE)

typedef enum { SB_POSIX, SB_POSIX_PAR, SB_BULK, SB_BULK_PAR, SB_CATALOG,
               SB_CATALOG_PIPE, SB_CATALOG_PARTITION, SB_METHOD_COUNT } SBMethod;
typedef enum { SB_ENUMERATE, SB_USAGE, SB_TREE } SBTask;
typedef enum { SB_LOGICAL, SB_ALLOCATED } SBSize;
typedef enum { SB_DFS, SB_FIFO, SB_RANDOM, SB_ID_BAND } SBOrder;
typedef enum { SB_LIVE, SB_QUIESCENT, SB_IMMUTABLE } SBConsistency;
typedef enum { SB_COMPLETE, SB_PARTIAL, SB_UNSUPPORTED, SB_FAILED } SBCompletion;
typedef enum { SB_UNVERIFIED, SB_VERIFIED, SB_MISMATCH } SBVerification;
typedef enum { SB_CACHE_WARM, SB_CACHE_FIRST, SB_CACHE_REBOOT, SB_CACHE_REMOUNT } SBCache;
typedef enum { SB_REDUCE_SORT, SB_REDUCE_HASH } SBReduce;

/* Fixed-size, private parent/child protocol. Not an on-disk/public ABI. */
typedef struct {
    char root[SB_PATH_CAP];
    SBMethod method;
    SBTask task;
    SBSize size;
    SBOrder order;
    SBConsistency consistency;
    SBCache cache;
    SBReduce reduce;
    uint32_t workers, frontier, id_shift, queue_limit, fd_limit;
    uint32_t round, config_index, catalog_retries, catalog_partitions;
    uint64_t buffer_size, memory_limit, seed, timeout_ns;
    uint64_t session;
    bool pack_invalid, skip_empty, adaptive, allow_volume_scan, keep_manifest;
    bool extra_entrycount, extra_linkcount, extra_uuid, omit_objtype;
    bool diagnostic;
} SBConfig;

typedef struct {
    uint64_t device, object_id, parent_id, name_offset, size;
    uint64_t parent_index, subtree_bytes, subtree_unknown;
    uint32_t name_length, kind, valid, reserved;
} SBEntry;

typedef struct {
    uint64_t device, object_id, size;
    uint32_t kind, valid;
} SBObject;

typedef struct {
    SBEntry *entries;
    size_t count, capacity;
    unsigned char *names;
    size_t names_size, names_capacity;
    SBObject *objects;
    size_t objects_count, objects_capacity;
    uint64_t digest_a, digest_b, digest_c;
    uint64_t files, directories, symlinks, other, bytes, unknown_sizes;
    bool retain;
} SBCollector;

typedef struct {
    uint64_t readdir_calls, stat_calls, bulk_calls, search_calls, opens, reopens;
    uint64_t enrichment_calls, batches, batch_entries, max_batch, metadata_bytes;
    uint64_t excluded_boundaries, empty_opens_avoided, errors, permission_errors;
    uint64_t vanished, identity_races, malformed_records, duplicates, missing_parents;
    uint64_t unknown_attributes, queue_peak, catalog_restarts, adaptive_changes;
    uint64_t search_ns, traversal_ns;
} SBCounters;

typedef struct {
    uint64_t total_ns, setup_ns, scan_ns, finalize_ns, process_wall_ns;
    uint64_t user_ns, system_ns, max_rss_bytes, managed_peak_bytes;
    uint64_t root_device, root_id, entries, files, directories, symlinks, other;
    uint64_t unique_objects, unique_files, bytes, unique_bytes, unknown_sizes;
    uint64_t digest_a, digest_b, digest_c, source_entries, graph_root_bytes;
    uint64_t timestamp_ns, environment_id, group_id;
    uint32_t uid, gid, cpus, actual_workers;
    SBCompletion completion;
    SBVerification verification;
    int error_code;
    bool volume_root, readonly_mount, graph_valid;
    char reason[256], verification_basis[48], os[160], machine[80], filesystem[64];
    char build[768], scope_note[160], hardware_model[80], product_version[48];
    uint64_t physical_memory_bytes;
    SBCounters counters;
} SBResult;

typedef struct {
    SBEntry *entries;
    size_t count;
    unsigned char *names;
    size_t names_size;
    uint64_t root_device, root_id;
    SBTask task;
    SBSize size;
} SBInventory;

typedef struct SBPath {
    atomic_uint refs;
    struct SBPath *parent;
    uint64_t device, id;
    size_t depth;
    char name[];
} SBPath;

typedef struct { SBPath *path; int fd; bool reserved_fd; } SBDirTask;
typedef struct { uint64_t device, id; bool occupied; } SBSeenSlot;

struct SBWalk;
typedef struct {
    struct SBWalk *walk;
    uint32_t index;
    SBCollector collector;
    SBCounters counters;
    unsigned char *buffer;
} SBWorker;

typedef struct SBWalk {
    const SBConfig *config;
    SBResult *result;
    int root_fd;
    SBPath *root_path;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    SBDirTask *queue;
    size_t queue_count, queue_capacity, queue_head;
    uint32_t active, worker_count, active_limit;
    uint64_t rng, last_band, last_adapt_ns, last_adapt_entries;
    double previous_rate;
    unsigned plateau;
    SBSeenSlot *seen;
    size_t seen_count, seen_capacity;
    uint64_t *mount_ids;
    size_t mount_count;
    atomic_uint queued_fds;
    uint32_t queued_fd_budget;
    atomic_bool stop;
    atomic_uint_fast64_t progress;
    SBWorker *workers;
    uint64_t deadline;
} SBWalk;

/* utility */
/* Entries enumerated by the running walk; readable by progress observers. */
extern atomic_uint_fast64_t sb_progress_entries;
_Noreturn void sb_die(const char *fmt, ...);
void sb_mem_limit(uint64_t bytes);
uint64_t sb_mem_peak(void);
void *sb_alloc(size_t n);
void *sb_calloc(size_t n, size_t size);
void *sb_realloc(void *p, size_t n);
void sb_free(void *p);
uint64_t sb_now_ns(void);
uint64_t sb_realtime_ns(void);
uint64_t sb_random(uint64_t *state);
uint64_t sb_mix(uint64_t n);
uint64_t sb_hash_bytes(const void *data, size_t n, uint64_t seed);
bool sb_add_u64(uint64_t *a, uint64_t b);
bool sb_write_all(int fd, const void *data, size_t n);
bool sb_read_all(int fd, void *data, size_t n);
uint32_t sb_kind(mode_t mode);
const char *sb_method_name(SBMethod m);
const char *sb_task_name(SBTask t);
const char *sb_size_name(SBSize s);
const char *sb_order_name(SBOrder o);
const char *sb_completion_name(SBCompletion c);
const char *sb_verification_name(SBVerification v);
const char *sb_cache_name(SBCache c);
const char *sb_consistency_name(SBConsistency c);
void sb_config_key(const SBConfig *c, char *out, size_t n);
void sb_failure(SBResult *r, SBCompletion c, int err, const char *fmt, ...);
void sb_counter_merge(SBCounters *to, const SBCounters *from);
void sb_environment(const SBConfig *c, SBResult *r, int root_fd);

/* collection and exact manifests */
void sb_collector_init(SBCollector *c, bool retain);
void sb_collector_destroy(SBCollector *c);
void sb_collect(SBCollector *c, const SBConfig *cfg, SBEntry e, const void *name);
void sb_collect_finalize(SBCollector *cs, size_t n, const SBConfig *cfg,
                         SBResult *r, SBInventory *inventory);
void sb_inventory_destroy(SBInventory *i);
void sb_inventory_sort(SBInventory *i);
bool sb_inventory_equal(SBInventory *a, SBInventory *b, char *reason, size_t n);
bool sb_manifest_write(int fd, const SBInventory *i);
bool sb_manifest_read(int fd, SBInventory *i, uint64_t memory_limit);
void sb_manifest_json(FILE *f, const SBInventory *i);

/* traversal */
void sb_walk_run(const SBConfig *c, SBResult *r, SBInventory *i);
void sb_posix_directory(SBWorker *w, SBDirTask *task);
void sb_bulk_directory(SBWorker *w, SBDirTask *task);
void sb_walk_entry(SBWorker *w, SBDirTask *parent, int parent_fd, SBEntry e,
                   const char *name, bool known_empty);
void sb_worker_error(SBWorker *w, int err);
void sb_walk_stop(SBWalk *w, int err, const char *reason);
bool sb_walk_expired(SBWalk *w);
int sb_reopen_dir(SBWorker *w, SBPath *path);
int sb_allocated_at(int dirfd, const char *name, uint64_t expected_id,
                     uint64_t expected_dev, uint64_t *size);

/* catalog */
void sb_catalog_run(const SBConfig *c, SBResult *r, SBInventory *i);
void sb_catalog_filter(SBCollector *c, const SBConfig *cfg, SBResult *r);

/* execution, CLI and report */
int sb_execute(const SBConfig *c, SBResult *r, SBInventory *inventory);
int sb_spawn(const char *self, const SBConfig *c, SBResult *r, SBInventory *i);
void sb_json_string(FILE *f, const char *s);
void sb_json_hex(FILE *f, const unsigned char *s, size_t n);
void sb_result_json(FILE *f, const SBConfig *c, const SBResult *r, const char *event);
int sb_report(const char *path);
int sb_probe(const SBConfig *c, FILE *out);
int sb_replay(const char *manifest, SBConfig *cfg, unsigned rounds, FILE *out);
#endif
