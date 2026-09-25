#include "scanbench.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif

static void load_mount_boundaries(SBWalk *w) {
#ifdef __APPLE__
    struct statfs *mounts;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0) return;
    const char *root = w->config->root;
    size_t length = strlen(root);
    w->mount_ids = sb_alloc((size_t)count * sizeof(*w->mount_ids));
    for (int j = 0; j < count; ++j) {
        const char *path = mounts[j].f_mntonname;
        if (!strcmp(root, path) || strncmp(root, path, length) ||
            (length > 1 && path[length] != '/')) continue;
        struct stat st;
        if (!lstat(path, &st) && S_ISDIR(st.st_mode) &&
            (uint64_t)st.st_dev == w->result->root_device)
            w->mount_ids[w->mount_count++] = (uint64_t)st.st_ino;
    }
#else
    (void)w;
#endif
}

static SBPath *path_new(SBPath *parent, const char *name, size_t length, uint64_t dev, uint64_t id) {
    if (length > SIZE_MAX - sizeof(SBPath) - 1) sb_die("path allocation overflow");
    SBPath *p = sb_alloc(sizeof(*p) + length + 1);
    atomic_init(&p->refs, 1); p->parent = parent; p->device = dev; p->id = id;
    p->depth = parent ? parent->depth + 1 : 0;
    if (parent) atomic_fetch_add(&parent->refs, 1);
    memcpy(p->name, name, length); p->name[length] = 0; return p;
}
static void path_release(SBPath *p) {
    /* Iterative: a deeply nested corpus cannot overflow the C stack. */
    while (p && atomic_fetch_sub(&p->refs, 1) == 1) {
        SBPath *parent = p->parent; sb_free(p); p = parent;
    }
}
static void seen_grow(SBWalk *w) {
    size_t cap = w->seen_capacity ? w->seen_capacity * 2 : 1024;
    if (cap < w->seen_capacity || cap > SIZE_MAX / sizeof(SBSeenSlot)) sb_die("seen-directory table overflow");
    SBSeenSlot *slots = sb_calloc(cap, sizeof(*slots));
    for (size_t j = 0; j < w->seen_capacity; ++j) if (w->seen[j].occupied) {
        SBSeenSlot e = w->seen[j]; size_t at = (size_t)sb_mix(e.id ^ sb_mix(e.device)) & (cap - 1);
        while (slots[at].occupied) at = (at + 1) & (cap - 1);
        slots[at] = e;
    }
    sb_free(w->seen); w->seen = slots; w->seen_capacity = cap;
}
static bool seen_insert(SBWalk *w, uint64_t dev, uint64_t id) {
    if (!w->seen_capacity || w->seen_count >= w->seen_capacity / 2) seen_grow(w);
    size_t at = (size_t)sb_mix(id ^ sb_mix(dev)) & (w->seen_capacity - 1);
    while (w->seen[at].occupied) {
        if (w->seen[at].device == dev && w->seen[at].id == id) return false;
        at = (at + 1) & (w->seen_capacity - 1);
    }
    w->seen[at] = (SBSeenSlot){dev, id, true}; ++w->seen_count; return true;
}
void sb_worker_error(SBWorker *w, int err) {
    ++w->counters.errors;
    if (err == EACCES || err == EPERM) ++w->counters.permission_errors;
    else if (err == ENOENT) ++w->counters.vanished;
    else if (err == ESTALE || err == ELOOP || err == EXDEV) ++w->counters.identity_races;
    pthread_mutex_lock(&w->walk->lock);
    sb_failure(w->walk->result, SB_PARTIAL, err, "entry/directory operation failed: %s", strerror(err));
    pthread_mutex_unlock(&w->walk->lock);
}
void sb_walk_stop(SBWalk *w, int err, const char *reason) {
    pthread_mutex_lock(&w->lock);
    sb_failure(w->result, SB_FAILED, err, "%s", reason);
    atomic_store(&w->stop, true); pthread_cond_broadcast(&w->ready);
    pthread_mutex_unlock(&w->lock);
}
bool sb_walk_expired(SBWalk *w) {
    if (atomic_load(&w->stop)) return true;
    if (w->deadline && sb_now_ns() >= w->deadline) {
        sb_walk_stop(w, ETIMEDOUT, "scan deadline exceeded"); return true;
    }
    return false;
}
static bool reserve_fd(SBWalk *w) {
    unsigned n = atomic_load(&w->queued_fds);
    while (n < w->queued_fd_budget) {
        if (atomic_compare_exchange_weak(&w->queued_fds, &n, n + 1)) return true;
    }
    return false;
}
static int checked_open(SBWorker *worker, int parent, const SBPath *p) {
    ++worker->counters.opens;
    int fd;
    do { fd = openat(parent, p->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return -1;
    struct stat st; ++worker->counters.stat_calls;
    int rc;
    do { rc = fstat(fd, &st); } while (rc < 0 && errno == EINTR);
    if (rc) { int e = errno; close(fd); errno = e; return -1; }
    if ((uint64_t)st.st_ino != p->id || (uint64_t)st.st_dev != p->device || !S_ISDIR(st.st_mode)) {
        close(fd); errno = ESTALE; return -1;
    }
    return fd;
}
int sb_reopen_dir(SBWorker *w, SBPath *path) {
    SBPath **components = sb_alloc((path->depth ? path->depth : 1) * sizeof(*components));
    SBPath *p = path; size_t n = 0;
    while (p->parent) { components[n++] = p; p = p->parent; }
    /* Open '.' rather than dup: dup would share the root directory cursor. */
    ++w->counters.opens; ++w->counters.reopens;
    int fd;
    do { fd = openat(w->walk->root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); } while (fd < 0 && errno == EINTR);
    for (size_t j = n; fd >= 0 && j; --j) {
        int next = checked_open(w, fd, components[j - 1]); int err = errno;
        close(fd); fd = next; errno = err;
    }
    sb_free(components); return fd;
}
static bool enqueue(SBWalk *w, SBDirTask task) {
    pthread_mutex_lock(&w->lock);
    if (atomic_load(&w->stop)) { pthread_mutex_unlock(&w->lock); return false; }
    if (w->queue_count >= w->config->queue_limit) {
        sb_failure(w->result, SB_FAILED, ENOBUFS, "directory queue limit exceeded; increase --queue-limit (no entries silently dropped)");
        atomic_store(&w->stop, true); pthread_cond_broadcast(&w->ready); pthread_mutex_unlock(&w->lock); return false;
    }
    if (w->queue_count == w->queue_capacity) {
        size_t n = w->queue_capacity ? w->queue_capacity * 2 : 256;
        if (n > w->config->queue_limit) n = w->config->queue_limit;
        SBDirTask *grown = sb_alloc(n * sizeof(*grown));
        for (size_t j = 0; j < w->queue_count; ++j)
            grown[j] = w->queue[(w->queue_head + j) % w->queue_capacity];
        sb_free(w->queue); w->queue = grown; w->queue_capacity = n; w->queue_head = 0;
    }
    w->queue[(w->queue_head + w->queue_count) % w->queue_capacity] = task;
    ++w->queue_count;
    if (w->queue_count > w->result->counters.queue_peak) w->result->counters.queue_peak = w->queue_count;
    pthread_cond_broadcast(&w->ready); pthread_mutex_unlock(&w->lock); return true;
}
static void dispose_task(SBWalk *w, SBDirTask *task) {
    if (task->fd >= 0) close(task->fd);
    if (task->reserved_fd) atomic_fetch_sub(&w->queued_fds, 1);
    path_release(task->path);
}
void sb_walk_entry(SBWorker *w, SBDirTask *parent, int parent_fd, SBEntry e,
                   const char *name, bool known_empty) {
    SBWalk *walk = w->walk;
    if (atomic_load(&walk->stop)) return;
    if (e.device != walk->result->root_device) { ++w->counters.excluded_boundaries; return; }
    /* APFS's merged startup namespace can give System and Data the same
     * st_dev. Exclude nested mounts by identity as well, so the Data mount
     * cannot introduce a second copy of the firmlinked directory graph. */
    if (e.kind == 2) for (size_t j = 0; j < walk->mount_count; ++j) {
        if (e.object_id == walk->mount_ids[j]) { ++w->counters.excluded_boundaries; return; }
    }
    e.parent_id = parent->path->id; e.valid |= SB_VALID_PARENT;
    sb_collect(&w->collector, walk->config, e, name);
    if (e.kind != 2) return;
    if (known_empty) { ++w->counters.empty_opens_avoided; return; }
    pthread_mutex_lock(&walk->lock);
    bool unseen = seen_insert(walk, e.device, e.object_id);
    pthread_mutex_unlock(&walk->lock);
    if (!unseen) { ++w->counters.duplicates; sb_worker_error(w, ELOOP); return; }
    SBDirTask task = {.path = path_new(parent->path, name, e.name_length, e.device, e.object_id), .fd = -1, .reserved_fd = false};
    if (reserve_fd(walk)) {
        task.reserved_fd = true; task.fd = checked_open(w, parent_fd, task.path);
        if (task.fd < 0) {
            int err = errno;
            atomic_fetch_sub(&walk->queued_fds, 1); task.reserved_fd = false;
            if (err != EMFILE && err != ENFILE) { sb_worker_error(w, err); path_release(task.path); return; }
            /* Descriptor pressure defers safely to a checked component-wise reopen. */
        }
    }
    if (!enqueue(walk, task)) dispose_task(walk, &task);
}
static void adapt(SBWalk *w) {
    if (!w->config->adaptive) return;
    uint64_t now = sb_now_ns();
    if (now - w->last_adapt_ns < UINT64_C(50000000)) return;
    uint64_t entries = atomic_load(&w->progress);
    double rate = (double)(entries - w->last_adapt_entries) / (double)(now - w->last_adapt_ns);
    unsigned before = w->active_limit;
    if (w->queue_count > (size_t)w->active_limit * 2) {
        if (w->previous_rate > 0 && rate < w->previous_rate * .70) {
            if (++w->plateau >= 2 && w->active_limit > 1) { --w->active_limit; w->plateau = 0; }
        } else if (w->active_limit < w->worker_count) { ++w->active_limit; w->plateau = 0; }
    } else if (!w->queue_count && w->active_limit > 1) { --w->active_limit; }
    if (before != w->active_limit) { ++w->result->counters.adaptive_changes; pthread_cond_broadcast(&w->ready); }
    w->previous_rate = rate; w->last_adapt_ns = now; w->last_adapt_entries = entries;
}
static bool dequeue(SBWorker *worker, SBDirTask *out) {
    SBWalk *w = worker->walk; pthread_mutex_lock(&w->lock);
    for (;;) {
        if (atomic_load(&w->stop)) { pthread_mutex_unlock(&w->lock); return false; }
        if (!w->queue_count && !w->active) {
            atomic_store(&w->stop, true); pthread_cond_broadcast(&w->ready); pthread_mutex_unlock(&w->lock); return false;
        }
        adapt(w);
        if (w->queue_count && worker->index < w->active_limit) break;
        pthread_cond_wait(&w->ready, &w->lock);
    }
    size_t at = w->queue_count - 1;
    if (w->config->order == SB_FIFO) at = 0;
    else if (w->config->order == SB_RANDOM) at = (size_t)(sb_random(&w->rng) % w->queue_count);
    else if (w->config->order == SB_ID_BAND) {
        size_t k = w->config->frontier < w->queue_count ? w->config->frontier : w->queue_count;
        size_t first = w->queue_count - k;
        uint64_t distance = UINT64_MAX;
        for (size_t j = first; j < w->queue_count; ++j) {
            uint64_t band = w->queue[(w->queue_head + j) % w->queue_capacity].path->id >> w->config->id_shift;
            uint64_t d = band > w->last_band ? band - w->last_band : w->last_band - band;
            if (d < distance) { distance = d; at = j; }
        }
        w->last_band = w->queue[(w->queue_head + at) % w->queue_capacity].path->id >> w->config->id_shift;
    }
    size_t physical = (w->queue_head + at) % w->queue_capacity;
    *out = w->queue[physical];
    if (w->config->order == SB_FIFO) w->queue_head = (w->queue_head + 1) % w->queue_capacity;
    else w->queue[physical] = w->queue[(w->queue_head + w->queue_count - 1) % w->queue_capacity];
    --w->queue_count; ++w->active;
    pthread_mutex_unlock(&w->lock); return true;
}
static void *run_worker(void *arg) {
    SBWorker *worker = arg; SBWalk *w = worker->walk; SBDirTask task;
    while (dequeue(worker, &task)) {
        if (task.fd < 0) {
            task.fd = sb_reopen_dir(worker, task.path);
            if (task.fd < 0) sb_worker_error(worker, errno);
        }
        if (task.fd >= 0 && !sb_walk_expired(w)) {
            uint64_t old_count = worker->collector.count;
            if (w->config->method == SB_BULK || w->config->method == SB_BULK_PAR) sb_bulk_directory(worker, &task);
            else sb_posix_directory(worker, &task);
            if (w->config->method != SB_BULK && w->config->method != SB_BULK_PAR) {
                atomic_fetch_add(&w->progress, worker->collector.count - old_count);
                atomic_fetch_add_explicit(&sb_progress_entries, worker->collector.count - old_count, memory_order_relaxed);
            }
        }
        dispose_task(w, &task);
        pthread_mutex_lock(&w->lock); --w->active; pthread_cond_broadcast(&w->ready); pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}
void sb_walk_run(const SBConfig *cfg, SBResult *r, SBInventory *inventory) {
    SBWalk w; memset(&w, 0, sizeof(w)); w.config = cfg; w.result = r;
    w.root_fd = open(cfg->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (w.root_fd < 0) { sb_failure(r, SB_FAILED, errno, "cannot open root: %s", strerror(errno)); return; }
    sb_environment(cfg, r, w.root_fd); ++r->counters.opens;
    w.worker_count = cfg->method == SB_POSIX || cfg->method == SB_BULK ? 1 : cfg->workers;
    r->actual_workers = w.worker_count;
    struct rlimit rl; uint64_t fd_limit = cfg->fd_limit;
    if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur != RLIM_INFINITY) {
        uint64_t available = rl.rlim_cur > 24 ? rl.rlim_cur - 24 : 0;
        if (available < fd_limit) fd_limit = available;
    }
    if (fd_limit < (uint64_t)w.worker_count * 2 + 2) {
        sb_failure(r, SB_FAILED, EMFILE, "file-descriptor budget too small for workers; lower --workers or raise the limit"); close(w.root_fd); return;
    }
    w.queued_fd_budget = (uint32_t)(fd_limit - w.worker_count * 2 - 1);
    atomic_init(&w.queued_fds, 0); atomic_init(&w.stop, false); atomic_init(&w.progress, 0);
    w.rng = cfg->seed; w.active_limit = cfg->adaptive ? (w.worker_count < 2 ? w.worker_count : 2) : w.worker_count;
    pthread_mutex_init(&w.lock, NULL); pthread_cond_init(&w.ready, NULL);
    load_mount_boundaries(&w);
    w.root_path = path_new(NULL, "", 0, r->root_device, r->root_id);
    seen_insert(&w, r->root_device, r->root_id);
    atomic_fetch_add(&w.root_path->refs, 1);
    enqueue(&w, (SBDirTask){.path = w.root_path, .fd = -1, .reserved_fd = false});
    w.workers = sb_calloc(w.worker_count, sizeof(*w.workers));
    for (uint32_t j = 0; j < w.worker_count; ++j) {
        w.workers[j].walk = &w; w.workers[j].index = j;
        sb_collector_init(&w.workers[j].collector, cfg->task == SB_TREE || cfg->keep_manifest);
        if (cfg->method == SB_BULK || cfg->method == SB_BULK_PAR) w.workers[j].buffer = sb_alloc((size_t)cfg->buffer_size);
    }
    uint64_t scan_start = sb_now_ns(); w.last_adapt_ns = scan_start;
    w.deadline = cfg->timeout_ns && scan_start <= UINT64_MAX - cfg->timeout_ns ? scan_start + cfg->timeout_ns : 0;
    r->setup_ns = scan_start - r->total_ns; /* total_ns temporarily holds start */
    pthread_t threads[SB_MAX_WORKERS]; uint32_t started = 0;
    if (w.worker_count == 1) run_worker(w.workers);
    else {
        pthread_attr_t attributes;
        pthread_attr_t *thread_attributes = NULL;
        if (!pthread_attr_init(&attributes)) {
            thread_attributes = &attributes;
#ifdef __APPLE__
            qos_class_t qos;
            int relative_priority;
            if (!pthread_get_qos_class_np(pthread_self(), &qos, &relative_priority) &&
                qos != QOS_CLASS_UNSPECIFIED)
                (void)pthread_attr_set_qos_class_np(&attributes, qos, relative_priority);
#endif
        }
        for (uint32_t j = 0; j < w.worker_count; ++j) {
            int err = pthread_create(&threads[j], thread_attributes, run_worker, w.workers + j);
            if (err) { sb_walk_stop(&w, err, "pthread_create failed"); break; }
            ++started;
        }
        if (thread_attributes) pthread_attr_destroy(thread_attributes);
        for (uint32_t j = 0; j < started; ++j) pthread_join(threads[j], NULL);
    }
    r->scan_ns = sb_now_ns() - scan_start;
    uint64_t finish_start = sb_now_ns();
    for (size_t j = 0; j < w.queue_count; ++j) dispose_task(&w, w.queue + ((w.queue_head + j) % w.queue_capacity));
    path_release(w.root_path); close(w.root_fd);
    sb_free(w.queue); sb_free(w.seen); sb_free(w.mount_ids);
    SBCollector *cs = sb_calloc(w.worker_count, sizeof(*cs));
    for (uint32_t j = 0; j < w.worker_count; ++j) {
        sb_counter_merge(&r->counters, &w.workers[j].counters);
        cs[j] = w.workers[j].collector; sb_free(w.workers[j].buffer);
    }
    sb_free(w.workers);
    pthread_mutex_destroy(&w.lock); pthread_cond_destroy(&w.ready);
    sb_collect_finalize(cs, w.worker_count, cfg, r, inventory); sb_free(cs);
    r->source_entries = r->entries; r->finalize_ns = sb_now_ns() - finish_start;
}
