#include "scan.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif

static void load_mount_boundaries(VSWalk *w) {
#ifdef __APPLE__
    struct statfs *mounts;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    if (count <= 0) {
        return;
    }
    const char *root = w->config->root;
    size_t length = strlen(root);
    w->mount_ids = vs_alloc((size_t)count * sizeof(*w->mount_ids));
    for (int j = 0; j < count; ++j) {
        const char *path = mounts[j].f_mntonname;
        if (!strcmp(root, path) || strncmp(root, path, length) ||
            (length > 1 && path[length] != '/')) {
            continue;
        }
        struct stat st;
        if (!lstat(path, &st) && S_ISDIR(st.st_mode) &&
            (uint64_t)st.st_dev == w->result->root_device) {
            w->mount_ids[w->mount_count++] = (uint64_t)st.st_ino;
        }
    }
#else
    (void)w;
#endif
}

static VSPath *path_new(VSPath *parent, const char *name, size_t length, uint64_t dev,
                        uint64_t id) {
    if (length > SIZE_MAX - sizeof(VSPath) - 1) {
        vs_die("path allocation overflow");
    }
    VSPath *p = vs_alloc(sizeof(*p) + length + 1);
    atomic_init(&p->refs, 1);
    p->parent = parent;
    p->device = dev;
    p->id = id;
    p->depth = parent ? parent->depth + 1 : 0;
    if (parent) {
        atomic_fetch_add(&parent->refs, 1);
    }
    memcpy(p->name, name, length);
    p->name[length] = 0;
    return p;
}

static void path_release(VSPath *p) {
    /* Iterative: a deeply nested corpus cannot overflow the C stack. */
    while (p && atomic_fetch_sub(&p->refs, 1) == 1) {
        VSPath *parent = p->parent;
        vs_free(p);
        p = parent;
    }
}

static void seen_grow(VSWalk *w) {
    size_t cap = w->seen_capacity ? w->seen_capacity * 2 : 1024;
    if (cap < w->seen_capacity || cap > SIZE_MAX / sizeof(VSSeenSlot)) {
        vs_die("seen-directory table overflow");
    }
    VSSeenSlot *slots = vs_calloc(cap, sizeof(*slots));
    for (size_t j = 0; j < w->seen_capacity; ++j) {
        if (w->seen[j].occupied) {
            VSSeenSlot e = w->seen[j];
            size_t at = (size_t)vs_mix(e.id ^ vs_mix(e.device)) & (cap - 1);
            while (slots[at].occupied) {
                at = (at + 1) & (cap - 1);
            }
            slots[at] = e;
        }
    }
    vs_free(w->seen);
    w->seen = slots;
    w->seen_capacity = cap;
}

static bool seen_insert(VSWalk *w, uint64_t dev, uint64_t id) {
    if (!w->seen_capacity || w->seen_count >= w->seen_capacity / 2) {
        seen_grow(w);
    }
    size_t at = (size_t)vs_mix(id ^ vs_mix(dev)) & (w->seen_capacity - 1);
    while (w->seen[at].occupied) {
        if (w->seen[at].device == dev && w->seen[at].id == id) {
            return false;
        }
        at = (at + 1) & (w->seen_capacity - 1);
    }
    w->seen[at] = (VSSeenSlot){dev, id, true};
    ++w->seen_count;
    return true;
}

void vs_worker_error(VSWorker *w, int err) {
    ++w->counters.errors;
    if (err == EACCES || err == EPERM) {
        ++w->counters.permission_errors;
    } else if (err == ESTALE || err == ELOOP || err == EXDEV) {
        ++w->counters.identity_races;
    }
    pthread_mutex_lock(&w->walk->lock);
    vs_failure(w->walk->result, VS_PARTIAL, err, "entry/directory operation failed: %s",
               strerror(err));
    pthread_mutex_unlock(&w->walk->lock);
}

void vs_walk_stop(VSWalk *w, int err, const char *reason) {
    pthread_mutex_lock(&w->lock);
    vs_failure(w->result, VS_FAILED, err, "%s", reason);
    atomic_store(&w->stop, true);
    pthread_cond_broadcast(&w->ready);
    pthread_mutex_unlock(&w->lock);
}

bool vs_walk_expired(VSWalk *w) {
    if (atomic_load(&w->stop)) {
        return true;
    }
    if (w->deadline && vs_now_ns() >= w->deadline) {
        vs_walk_stop(w, ETIMEDOUT, "scan deadline exceeded");
        return true;
    }
    return false;
}

static bool reserve_fd(VSWalk *w) {
    unsigned n = atomic_load(&w->queued_fds);
    while (n < w->queued_fd_budget) {
        if (atomic_compare_exchange_weak(&w->queued_fds, &n, n + 1)) {
            return true;
        }
    }
    return false;
}

static int checked_open(int parent, const VSPath *p) {

    int fd;
    do {
        fd = openat(parent, p->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return -1;
    }
    struct stat st;

    int rc;
    do {
        rc = fstat(fd, &st);
    } while (rc < 0 && errno == EINTR);
    if (rc) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if ((uint64_t)st.st_ino != p->id || (uint64_t)st.st_dev != p->device || !S_ISDIR(st.st_mode)) {
        close(fd);
        errno = ESTALE;
        return -1;
    }
    return fd;
}

static int reopen_dir(VSWorker *w, VSPath *path) {
    VSPath **components = vs_alloc((path->depth ? path->depth : 1) * sizeof(*components));
    VSPath *p = path;
    size_t n = 0;
    while (p->parent) {
        components[n++] = p;
        p = p->parent;
    }
    /* Open '.' rather than dup: dup would share the root directory cursor. */

    int fd;
    do {
        fd = openat(w->walk->root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    for (size_t j = n; fd >= 0 && j; --j) {
        int next = checked_open(fd, components[j - 1]);
        int err = errno;
        close(fd);
        fd = next;
        errno = err;
    }
    vs_free(components);
    return fd;
}

static bool enqueue(VSWalk *w, VSDirTask task) {
    pthread_mutex_lock(&w->lock);
    if (atomic_load(&w->stop)) {
        pthread_mutex_unlock(&w->lock);
        return false;
    }
    if (w->queue_count >= w->config->queue_limit) {
        vs_failure(w->result, VS_FAILED, ENOBUFS, "directory queue limit exceeded");
        atomic_store(&w->stop, true);
        pthread_cond_broadcast(&w->ready);
        pthread_mutex_unlock(&w->lock);
        return false;
    }
    if (w->queue_count == w->queue_capacity) {
        size_t n = w->queue_capacity ? w->queue_capacity * 2 : 256;
        if (n > w->config->queue_limit) {
            n = w->config->queue_limit;
        }
        w->queue = vs_realloc(w->queue, n * sizeof(*w->queue));
        w->queue_capacity = n;
    }
    w->queue[w->queue_count] = task;
    ++w->queue_count;
    pthread_cond_broadcast(&w->ready);
    pthread_mutex_unlock(&w->lock);
    return true;
}

static void dispose_task(VSWalk *w, VSDirTask *task) {
    if (task->fd >= 0) {
        close(task->fd);
    }
    if (task->reserved_fd) {
        atomic_fetch_sub(&w->queued_fds, 1);
    }
    path_release(task->path);
}

void vs_walk_entry(VSWorker *w, VSDirTask *parent, int parent_fd, VSEntry e, const char *name) {
    VSWalk *walk = w->walk;
    if (atomic_load(&walk->stop)) {
        return;
    }
    if (e.device != walk->result->root_device) {
        ++w->counters.excluded_boundaries;
        return;
    }
    /* APFS's merged startup namespace can give System and Data the same
     * st_dev. Exclude nested mounts by identity as well, so the Data mount
     * cannot introduce a second copy of the firmlinked directory graph. */
    if (e.kind == 2) {
        for (size_t j = 0; j < walk->mount_count; ++j) {
            if (e.object_id == walk->mount_ids[j]) {
                ++w->counters.excluded_boundaries;
                return;
            }
        }
    }
    e.parent_id = parent->path->id;
    e.valid |= VS_VALID_PARENT;
    vs_collect(&w->collector, e, name);
    if (e.kind != 2) {
        return;
    }
    pthread_mutex_lock(&walk->lock);
    bool unseen = seen_insert(walk, e.device, e.object_id);
    pthread_mutex_unlock(&walk->lock);
    if (!unseen) {
        ++w->counters.duplicates;
        vs_worker_error(w, ELOOP);
        return;
    }
    VSDirTask task = {.path = path_new(parent->path, name, e.name_length, e.device, e.object_id),
                      .fd = -1,
                      .reserved_fd = false};
    if (reserve_fd(walk)) {
        task.reserved_fd = true;
        task.fd = checked_open(parent_fd, task.path);
        if (task.fd < 0) {
            int err = errno;
            atomic_fetch_sub(&walk->queued_fds, 1);
            task.reserved_fd = false;
            if (err != EMFILE && err != ENFILE) {
                vs_worker_error(w, err);
                path_release(task.path);
                return;
            }
            /* Descriptor pressure defers safely to a checked component-wise reopen. */
        }
    }
    if (!enqueue(walk, task)) {
        dispose_task(walk, &task);
    }
}

static bool dequeue(VSWorker *worker, VSDirTask *out) {
    VSWalk *w = worker->walk;
    pthread_mutex_lock(&w->lock);
    for (;;) {
        if (atomic_load(&w->stop)) {
            pthread_mutex_unlock(&w->lock);
            return false;
        }
        if (!w->queue_count && !w->active) {
            atomic_store(&w->stop, true);
            pthread_cond_broadcast(&w->ready);
            pthread_mutex_unlock(&w->lock);
            return false;
        }
        if (w->queue_count) {
            break;
        }
        pthread_cond_wait(&w->ready, &w->lock);
    }
    *out = w->queue[--w->queue_count];
    ++w->active;
    pthread_mutex_unlock(&w->lock);
    return true;
}

static void *run_worker(void *arg) {
    VSWorker *worker = arg;
    VSWalk *w = worker->walk;
    VSDirTask task;
    while (dequeue(worker, &task)) {
        if (task.fd < 0) {
            task.fd = reopen_dir(worker, task.path);
            if (task.fd < 0) {
                vs_worker_error(worker, errno);
            }
        }
        if (task.fd >= 0 && !vs_walk_expired(w)) {
            uint64_t old_count = worker->collector.count;
            if (w->config->method == VS_BULK) {
                vs_bulk_directory(worker, &task);
            } else {
                vs_posix_directory(worker, &task);
            }
            if (w->config->method != VS_BULK) {
                atomic_fetch_add_explicit(&vs_progress_entries, worker->collector.count - old_count,
                                          memory_order_relaxed);
            }
        }
        dispose_task(w, &task);
        pthread_mutex_lock(&w->lock);
        --w->active;
        pthread_cond_broadcast(&w->ready);
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}

void vs_walk_run(const VSConfig *cfg, VSResult *r, VSInventory *inventory) {
    VSWalk w;
    memset(&w, 0, sizeof(w));
    w.config = cfg;
    w.result = r;
    w.root_fd = open(cfg->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (w.root_fd < 0) {
        vs_failure(r, VS_FAILED, errno, "cannot open root: %s", strerror(errno));
        return;
    }
    struct stat st;
    if (fstat(w.root_fd, &st)) {
        vs_failure(r, VS_FAILED, errno, "cannot stat root: %s", strerror(errno));
        close(w.root_fd);
        return;
    }
    r->root_device = (uint64_t)st.st_dev;
    r->root_id = (uint64_t)st.st_ino;

    w.worker_count = cfg->workers;
    r->actual_workers = w.worker_count;
    struct rlimit rl;
    uint64_t fd_limit = cfg->fd_limit;
    if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur != RLIM_INFINITY) {
        uint64_t available = rl.rlim_cur > 24 ? rl.rlim_cur - 24 : 0;
        if (available < fd_limit) {
            fd_limit = available;
        }
    }
    if (fd_limit < (uint64_t)w.worker_count * 2 + 2) {
        vs_failure(
            r, VS_FAILED, EMFILE,
            "file-descriptor budget too small for workers; lower --workers or raise the limit");
        close(w.root_fd);
        return;
    }
    w.queued_fd_budget = (uint32_t)(fd_limit - w.worker_count * 2 - 1);
    atomic_init(&w.queued_fds, 0);
    atomic_init(&w.stop, false);
    pthread_mutex_init(&w.lock, NULL);
    pthread_cond_init(&w.ready, NULL);
    load_mount_boundaries(&w);
    w.root_path = path_new(NULL, "", 0, r->root_device, r->root_id);
    seen_insert(&w, r->root_device, r->root_id);
    atomic_fetch_add(&w.root_path->refs, 1);
    enqueue(&w, (VSDirTask){.path = w.root_path, .fd = -1, .reserved_fd = false});
    w.workers = vs_calloc(w.worker_count, sizeof(*w.workers));
    for (uint32_t j = 0; j < w.worker_count; ++j) {
        w.workers[j].walk = &w;
        vs_collector_init(&w.workers[j].collector);
        if (cfg->method == VS_BULK) {
            w.workers[j].buffer = vs_alloc((size_t)cfg->buffer_size);
        }
    }
    uint64_t scan_start = vs_now_ns();
    w.deadline = cfg->timeout_ns && scan_start <= UINT64_MAX - cfg->timeout_ns
                     ? scan_start + cfg->timeout_ns
                     : 0;
    pthread_t threads[VS_MAX_WORKERS];
    uint32_t started = 0;
    if (w.worker_count == 1) {
        run_worker(w.workers);
    } else {
        pthread_attr_t attributes;
        pthread_attr_t *thread_attributes = NULL;
        if (!pthread_attr_init(&attributes)) {
            thread_attributes = &attributes;
#ifdef __APPLE__
            qos_class_t qos;
            int relative_priority;
            if (!pthread_get_qos_class_np(pthread_self(), &qos, &relative_priority) &&
                qos != QOS_CLASS_UNSPECIFIED) {
                (void)pthread_attr_set_qos_class_np(&attributes, qos, relative_priority);
            }
#endif
        }
        for (uint32_t j = 0; j < w.worker_count; ++j) {
            int err = pthread_create(&threads[j], thread_attributes, run_worker, w.workers + j);
            if (err) {
                vs_walk_stop(&w, err, "pthread_create failed");
                break;
            }
            ++started;
        }
        if (thread_attributes) {
            pthread_attr_destroy(thread_attributes);
        }
        for (uint32_t j = 0; j < started; ++j) {
            pthread_join(threads[j], NULL);
        }
    }
    r->scan_ns = vs_now_ns() - scan_start;
    for (size_t j = 0; j < w.queue_count; ++j) {
        dispose_task(&w, w.queue + j);
    }
    path_release(w.root_path);
    close(w.root_fd);
    vs_free(w.queue);
    vs_free(w.seen);
    vs_free(w.mount_ids);
    VSCollector *cs = vs_calloc(w.worker_count, sizeof(*cs));
    for (uint32_t j = 0; j < w.worker_count; ++j) {
        vs_counter_merge(&r->counters, &w.workers[j].counters);
        cs[j] = w.workers[j].collector;
        vs_free(w.workers[j].buffer);
    }
    vs_free(w.workers);
    pthread_mutex_destroy(&w.lock);
    pthread_cond_destroy(&w.ready);
    vs_collect_finalize(cs, w.worker_count, r, inventory);
    vs_free(cs);
}
