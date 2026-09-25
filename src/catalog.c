#include "scanbench.h"
#include "attrs.h"
#include "darwin_api.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The namespace join is portable and tested independently of searchfs. */
typedef struct { uint64_t id, dev; size_t entry; unsigned char state; bool occupied; } DirSlot;
static size_t dir_find(DirSlot *v, size_t cap, uint64_t dev, uint64_t id) {
    size_t at = (size_t)sb_mix(id ^ sb_mix(dev)) & (cap - 1);
    while (v[at].occupied && (v[at].id != id || v[at].dev != dev)) at = (at + 1) & (cap - 1);
    return at;
}
void sb_catalog_filter(SBCollector *c, const SBConfig *cfg, SBResult *r) {
    size_t cap = 16;
    if (c->directories > SIZE_MAX / 2) sb_die("catalog directory map overflow");
    while (cap < (size_t)c->directories * 2) { if (cap > SIZE_MAX / 2) sb_die("catalog map overflow"); cap *= 2; }
    DirSlot *dirs = sb_calloc(cap, sizeof(*dirs));
    size_t *chain = sb_alloc((c->directories + 1) * sizeof(*chain));
    for (size_t j = 0; j < c->count; ++j) {
        SBEntry *e = c->entries + j;
        if (e->kind != 2) continue;
        size_t at = dir_find(dirs, cap, e->device, e->object_id);
        if (dirs[at].occupied) { ++r->counters.duplicates; sb_failure(r, SB_PARTIAL, EINVAL, "catalog has repeated directory identities"); continue; }
        dirs[at] = (DirSlot){e->object_id, e->device, j, 0, true};
        if (e->reserved) dirs[at].state = 2; /* explicit boundary exclusion */
        else if (e->device == r->root_device && e->object_id == r->root_id) dirs[at].state = 1;
    }
    for (size_t j = 0; j < cap; ++j) {
        if (!dirs[j].occupied || dirs[j].state) continue;
        size_t at = j, n = 0; unsigned char state = 2;
        for (;;) {
            if (dirs[at].state) {
                if (dirs[at].state == 3) { ++r->counters.identity_races; sb_failure(r, SB_PARTIAL, ELOOP, "catalog parent cycle"); state = 2; }
                else state = dirs[at].state;
                break;
            }
            dirs[at].state = 3; chain[n++] = at;
            SBEntry *e = c->entries + dirs[at].entry;
            if (e->device == r->root_device && e->parent_id == r->root_id) { state = 1; break; }
            size_t parent = dir_find(dirs, cap, e->device, e->parent_id);
            if (!dirs[parent].occupied) {
                if (r->volume_root && e->device == r->root_device) {
                    ++r->counters.missing_parents;
                    sb_failure(r, SB_PARTIAL, ENOENT, "catalog contains a directory with an unresolved parent");
                }
                state = 2; break;
            }
            at = parent;
        }
        while (n) dirs[chain[--n]].state = state;
    }
    SBCollector selected; sb_collector_init(&selected, cfg->task == SB_TREE || cfg->keep_manifest);
    r->source_entries = c->count;
    for (size_t j = 0; j < c->count; ++j) {
        SBEntry e = c->entries[j]; bool in = false;
        if (e.device != r->root_device || e.reserved) { ++r->counters.excluded_boundaries; continue; }
        if (e.object_id == r->root_id && e.kind == 2) continue; /* anchor, not descendant */
        if (e.parent_id == r->root_id) in = true;
        else {
            size_t at = dir_find(dirs, cap, e.device, e.parent_id);
            in = dirs[at].occupied && dirs[at].state == 1;
            if (!dirs[at].occupied && r->volume_root) {
                ++r->counters.missing_parents;
                sb_failure(r, SB_PARTIAL, ENOENT, "catalog contains an entry with an unresolved parent");
            }
        }
        if (in) sb_collect(&selected, cfg, e, c->names + e.name_offset);
    }
    sb_collector_destroy(c); *c = selected; sb_free(chain); sb_free(dirs);
}

#if defined(__APPLE__) || defined(SB_TEST_DARWIN)
static void catalog_metadata_error(SBResult *r, int err) {
    ++r->counters.errors;
    if (err == EACCES || err == EPERM) ++r->counters.permission_errors;
    else if (err == ENOENT) ++r->counters.vanished;
    else if (err == ESTALE || err == ELOOP || err == EXDEV || err == ENOTDIR) ++r->counters.identity_races;
    sb_failure(r, SB_PARTIAL, err, "catalog metadata enrichment failed: %s", strerror(err));
}
static bool catalog_expired(SBResult *r, uint64_t deadline) {
    if (!deadline || sb_now_ns() < deadline) return false;
    sb_failure(r, SB_FAILED, ETIMEDOUT, "catalog scan/enrichment deadline exceeded");
    return true;
}
/* Reopen one parent from the pinned scan root, validating every component.
 * Root/current/next are the only descriptors: depth never increases the FD
 * requirement, and no path-length limit or symlink resolution is introduced. */
static int catalog_parent_fd(int root, const SBCollector *c, size_t parent,
                             size_t *chain, SBResult *r, uint64_t deadline) {
    size_t depth = 0;
    while (parent != c->count) {
        if (parent >= c->count || c->entries[parent].kind != 2) { errno = ENOENT; return -1; }
        if (depth >= c->directories) { errno = ELOOP; return -1; }
        chain[depth++] = parent;
        parent = (size_t)c->entries[parent].parent_index;
    }
    int fd = root;
    if (depth) ++r->counters.reopens;
    while (depth) {
        if (catalog_expired(r, deadline)) { if (fd != root) close(fd); errno = ETIMEDOUT; return -1; }
        const SBEntry *e = c->entries + chain[--depth];
        ++r->counters.opens;
        int next = openat(fd, (const char *)c->names + e->name_offset, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int err = errno;
        if (fd != root) close(fd);
        if (next < 0) { errno = err; return -1; }
        struct stat st; ++r->counters.stat_calls;
        if (fstat(next, &st)) { err = errno; close(next); errno = err; return -1; }
        if ((uint64_t)st.st_dev != e->device || (uint64_t)st.st_ino != e->object_id || !S_ISDIR(st.st_mode)) {
            close(next); errno = ESTALE; return -1;
        }
        fd = next;
    }
    return fd;
}
/* APFS searchfs can report every non-directory as VREG. Resolve types through
 * no-follow metadata, then rebuild accounting so link target lengths cannot
 * become file usage. Directory grouping amortizes checked parent reopening. */
static void enrich_catalog(SBCollector *c, const SBConfig *cfg, SBResult *r,
                           int root, uint64_t deadline) {
    uint64_t begin = cfg->diagnostic ? sb_now_ns() : 0;
    size_t cap = 16;
    if (c->directories >= SIZE_MAX / 2 || c->count == SIZE_MAX) sb_die("catalog metadata map overflow");
    while (cap < ((size_t)c->directories + 1) * 2) { if (cap > SIZE_MAX / 2) sb_die("catalog metadata map overflow"); cap *= 2; }
    DirSlot *dirs = sb_calloc(cap, sizeof(*dirs));
    size_t *heads = sb_calloc(c->count + 1, sizeof(*heads));
    size_t *next = sb_calloc(c->count, sizeof(*next));
    size_t *chain = sb_calloc((size_t)c->directories + 1, sizeof(*chain));
    SBCollector enriched; sb_collector_init(&enriched, cfg->task == SB_TREE || cfg->keep_manifest);
    size_t at = dir_find(dirs, cap, r->root_device, r->root_id);
    dirs[at] = (DirSlot){r->root_id, r->root_device, c->count, 0, true};
    for (size_t j = 0; j < c->count; ++j) {
        if (!(j & 255) && catalog_expired(r, deadline)) goto done;
        const SBEntry *e = c->entries + j;
        if (e->kind != 2) continue;
        at = dir_find(dirs, cap, e->device, e->object_id);
        if (!dirs[at].occupied) dirs[at] = (DirSlot){e->object_id, e->device, j, 0, true};
    }
    for (size_t j = 0; j < c->count; ++j) {
        if (!(j & 255) && catalog_expired(r, deadline)) goto done;
        SBEntry *e = c->entries + j;
        at = dir_find(dirs, cap, e->device, e->parent_id);
        e->parent_index = dirs[at].occupied ? dirs[at].entry : SB_NO_INDEX;
        if (!dirs[at].occupied) { ++r->counters.missing_parents; catalog_metadata_error(r, ENOENT); continue; }
        next[j] = heads[dirs[at].entry]; heads[dirs[at].entry] = j + 1;
    }
    for (size_t parent = 0; parent <= c->count; ++parent) {
        if (catalog_expired(r, deadline)) break;
        if (!heads[parent]) continue;
        int fd = catalog_parent_fd(root, c, parent, chain, r, deadline);
        if (fd < 0) { catalog_metadata_error(r, errno); continue; }
        for (size_t index = heads[parent]; index; index = next[index - 1]) {
            if (catalog_expired(r, deadline)) break;
            SBEntry e = c->entries[index - 1]; const char *name = (const char *)c->names + e.name_offset;
            struct stat st; ++r->counters.enrichment_calls; ++r->counters.stat_calls;
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) { catalog_metadata_error(r, errno); continue; }
            if ((uint64_t)st.st_dev != e.device || (uint64_t)st.st_ino != e.object_id ||
                (e.kind == 2) != (S_ISDIR(st.st_mode) != 0)) { catalog_metadata_error(r, ESTALE); continue; }
            e.kind = sb_kind(st.st_mode); e.size = 0; e.valid |= SB_VALID_SIZE;
            if (e.kind == 1 && cfg->task != SB_ENUMERATE) {
                if (cfg->size == SB_LOGICAL && st.st_size >= 0) e.size = (uint64_t)st.st_size;
                else if (cfg->size == SB_ALLOCATED) {
                    ++r->counters.enrichment_calls;
                    if (sb_allocated_at(fd, name, e.object_id, e.device, &e.size)) {
                        e.valid &= ~SB_VALID_SIZE; ++r->counters.unknown_attributes; catalog_metadata_error(r, errno);
                    }
                } else { e.valid &= ~SB_VALID_SIZE; ++r->counters.unknown_attributes; catalog_metadata_error(r, ENODATA); }
            }
            if (catalog_expired(r, deadline)) break;
            sb_collect(&enriched, cfg, e, name);
        }
        if (fd != root) close(fd);
    }
done:
    sb_free(chain); sb_free(next); sb_free(heads); sb_free(dirs);
    sb_collector_destroy(c); *c = enriched;
    if (cfg->diagnostic) r->counters.traversal_ns += sb_now_ns() - begin;
}
typedef struct { unsigned char *data; unsigned long count; bool reset; int state; } CatSlot;
typedef struct {
    const SBConfig *cfg;
    SBAttrSpec spec;
    SBCollector collector;
    SBCounters fetch_counts, parse_counts;
    uint64_t lower, upper, deadline;
    bool partition, pipelined;
    CatSlot slots[2];
    pthread_mutex_t lock;
    pthread_cond_t ready;
    unsigned next_read, next_write;
    bool done;
    atomic_bool cancel;
    int fetch_error, parse_error;
} CatJob;
static bool parse_batch(CatJob *j, const unsigned char *p, unsigned long n, bool reset) {
    if (reset) { sb_collector_destroy(&j->collector); sb_collector_init(&j->collector, true); return true; }
    size_t off = 0;
    for (unsigned long k = 0; k < n; ++k) {
        SBAttrRecord x;
        if (!sb_attr_decode(p + off, (size_t)j->cfg->buffer_size - off, j->spec, j->cfg->size, &x)) {
            ++j->parse_counts.malformed_records; j->parse_error = EIO; atomic_store(&j->cancel, true); return false;
        }
        off += x.record_size;
        if (!x.name || !x.entry.name_length || !strcmp(x.name, ".") || !strcmp(x.name, "..")) continue;
        if ((x.entry.valid & SB_REQUIRED) != SB_REQUIRED) {
            ++j->parse_counts.unknown_attributes; j->parse_error = ENOTSUP; atomic_store(&j->cancel, true); return false;
        }
        if (j->partition && (x.entry.object_id < j->lower || x.entry.object_id > j->upper)) {
            j->parse_error = EINVAL; atomic_store(&j->cancel, true); return false;
        }
        sb_collect(&j->collector, j->cfg, x.entry, x.name);
        if (x.entry.kind == 2 && ((x.has_flags && (x.flags & SB_F_FIRMLINK)) || (x.has_mountstatus && x.mountstatus)))
            j->collector.entries[j->collector.count - 1].reserved = 1;
    }
    j->parse_counts.metadata_bytes += off; return true;
}
static CatSlot *acquire_slot(CatJob *j) {
    CatSlot *slot = j->slots + (j->pipelined ? j->next_write : 0);
    if (!j->pipelined) return slot;
    pthread_mutex_lock(&j->lock);
    while (slot->state && !atomic_load(&j->cancel)) pthread_cond_wait(&j->ready, &j->lock);
    bool cancelled = atomic_load(&j->cancel);
    if (!cancelled) slot->state = 2;
    pthread_mutex_unlock(&j->lock); return cancelled ? NULL : slot;
}
static void publish_slot(CatJob *j, CatSlot *slot) {
    if (!j->pipelined) { parse_batch(j, slot->data, slot->count, slot->reset); return; }
    pthread_mutex_lock(&j->lock); slot->state = 1; j->next_write ^= 1;
    pthread_cond_broadcast(&j->ready); pthread_mutex_unlock(&j->lock);
}
static void *read_catalog(void *arg) {
    CatJob *j = arg;
    struct attrlist returns = {0}; returns.bitmapcount = ATTR_BIT_MAP_COUNT;
    returns.commonattr = j->spec.common; returns.dirattr = j->spec.dir; returns.fileattr = j->spec.file;
    struct fssearchblock search; memset(&search, 0, sizeof(search));
    search.returnattrs = &returns; search.returnbuffersize = (size_t)j->cfg->buffer_size;
    search.maxmatches = 65536; search.timelimit.tv_sec = 0; search.timelimit.tv_usec = 100000;
    search.searchattrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    unsigned char lower[16] = {0}, upper[16] = {0};
    uint32_t length = j->partition ? 12 : 16; memcpy(lower, &length, 4); memcpy(upper, &length, 4);
    unsigned opts = SRCHFS_START | SRCHFS_MATCHFILES | SRCHFS_MATCHDIRS;
    if (j->partition) {
        search.searchattrs.commonattr = ATTR_CMN_FILEID;
        memcpy(lower + 4, &j->lower, 8); memcpy(upper + 4, &j->upper, 8);
    } else {
        /* Empty substring matches every name; no hidden/package/link skip flags.
         * A zero-attribute predicate is NOT assumed to mean "match all". */
        search.searchattrs.commonattr = ATTR_CMN_NAME; opts |= SRCHFS_MATCHPARTIALNAMES;
        int32_t offset = 8; uint32_t bytes = 1;
        memcpy(lower + 4, &offset, 4); memcpy(lower + 8, &bytes, 4);
        memcpy(upper + 4, &offset, 4); memcpy(upper + 8, &bytes, 4);
    }
    search.searchparams1 = lower; search.searchparams2 = upper;
    search.sizeofsearchparams1 = length; search.sizeofsearchparams2 = length;
    struct searchstate state; memset(&state, 0, sizeof(state));
    unsigned retries = 0;
    for (;;) {
        if (atomic_load(&j->cancel)) break;
        if (j->deadline && sb_now_ns() >= j->deadline) { j->fetch_error = ETIMEDOUT; break; }
        CatSlot *slot = acquire_slot(j); if (!slot) break;
        search.returnbuffer = slot->data; slot->count = 0; slot->reset = false;
        uint64_t start = j->cfg->diagnostic ? sb_now_ns() : 0;
        ++j->fetch_counts.search_calls;
        int rc = searchfs(j->cfg->root, &search, &slot->count, 0x08000103u, opts, &state);
        int err = rc == 0 ? 0 : errno;
        if (j->cfg->diagnostic) j->fetch_counts.search_ns += sb_now_ns() - start;
        opts &= ~(unsigned)SRCHFS_START;
        if (err == EBUSY) {
            slot->count = 0; slot->reset = true; publish_slot(j, slot);
            if (retries++ >= j->cfg->catalog_retries) { j->fetch_error = EBUSY; break; }
            ++j->fetch_counts.catalog_restarts; memset(&state, 0, sizeof(state)); opts |= SRCHFS_START; continue;
        }
        if (err && err != EAGAIN) {
            slot->count = 0; publish_slot(j, slot); j->fetch_error = err; break;
        }
        ++j->fetch_counts.batches; j->fetch_counts.batch_entries += slot->count;
        if (slot->count > j->fetch_counts.max_batch) j->fetch_counts.max_batch = slot->count;
        publish_slot(j, slot);
        if (!err) break;
    }
    if (j->pipelined) { pthread_mutex_lock(&j->lock); j->done = true; pthread_cond_broadcast(&j->ready); pthread_mutex_unlock(&j->lock); }
    return NULL;
}
static void *run_catalog_job(void *arg) {
    CatJob *j = arg;
    if (!j->pipelined) { read_catalog(j); return NULL; }
    pthread_t producer;
    int err = pthread_create(&producer, NULL, read_catalog, j);
    if (err) { j->fetch_error = err; return NULL; }
    for (;;) {
        pthread_mutex_lock(&j->lock);
        CatSlot *slot = j->slots + j->next_read;
        while (slot->state != 1 && !j->done) pthread_cond_wait(&j->ready, &j->lock);
        if (slot->state != 1 && j->done) { pthread_mutex_unlock(&j->lock); break; }
        pthread_mutex_unlock(&j->lock);
        bool ok = parse_batch(j, slot->data, slot->count, slot->reset);
        pthread_mutex_lock(&j->lock); slot->state = 0; j->next_read ^= 1;
        pthread_cond_broadcast(&j->ready); pthread_mutex_unlock(&j->lock);
        if (!ok) break;
    }
    if (atomic_load(&j->cancel)) { pthread_mutex_lock(&j->lock); pthread_cond_broadcast(&j->ready); pthread_mutex_unlock(&j->lock); }
    pthread_join(producer, NULL); return NULL;
}
#endif
void sb_catalog_run(const SBConfig *cfg, SBResult *r, SBInventory *inventory) {
#if !defined(__APPLE__) && !defined(SB_TEST_DARWIN)
    (void)cfg; (void)inventory; sb_failure(r, SB_UNSUPPORTED, ENOTSUP, "searchfs requires macOS");
#else
    int fd = open(cfg->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { sb_failure(r, SB_FAILED, errno, "cannot open root: %s", strerror(errno)); return; }
    sb_environment(cfg, r, fd); ++r->counters.opens;
    if (!r->volume_root && !cfg->allow_volume_scan) { close(fd); sb_failure(r, SB_UNSUPPORTED, EINVAL, "catalog searches the whole volume; a subtree requires --allow-volume-scan"); return; }
    uint32_t n = cfg->method == SB_CATALOG_PARTITION ? cfg->catalog_partitions : 1;
    r->actual_workers = cfg->method == SB_CATALOG_PIPE ? 2 : n;
    CatJob *jobs = sb_calloc(n, sizeof(*jobs));
    uint64_t now = sb_now_ns();
    uint64_t deadline = cfg->timeout_ns && now <= UINT64_MAX - cfg->timeout_ns ? now + cfg->timeout_ns : 0;
    for (uint32_t j = 0; j < n; ++j) {
        CatJob *job = jobs + j; job->cfg = cfg; job->spec = sb_attr_spec(cfg, true);
        job->partition = cfg->method == SB_CATALOG_PARTITION; job->pipelined = cfg->method == SB_CATALOG_PIPE;
        job->deadline = deadline;
        if (n == 1) { job->lower = 0; job->upper = UINT64_MAX; }
        else {
            unsigned shift = n == 2 ? 63 : 62;
            job->lower = (uint64_t)j << shift;
            job->upper = j == n - 1 ? UINT64_MAX : (((uint64_t)j + 1) << shift) - 1;
        }
        atomic_init(&job->cancel, false); sb_collector_init(&job->collector, true);
        pthread_mutex_init(&job->lock, NULL); pthread_cond_init(&job->ready, NULL);
        job->slots[0].data = sb_alloc((size_t)cfg->buffer_size);
        if (job->pipelined) job->slots[1].data = sb_alloc((size_t)cfg->buffer_size);
    }
    uint64_t scan_start = sb_now_ns(); r->setup_ns = scan_start - r->total_ns;
    if (n == 1) run_catalog_job(jobs);
    else {
        pthread_t threads[4]; uint32_t started = 0;
        for (uint32_t j = 0; j < n; ++j) {
            int e = pthread_create(threads + j, NULL, run_catalog_job, jobs + j);
            if (e) { jobs[j].fetch_error = e; break; } ++started;
        }
        for (uint32_t j = 0; j < started; ++j) pthread_join(threads[j], NULL);
    }
    SBCollector all; sb_collector_init(&all, true);
    for (uint32_t j = 0; j < n; ++j) {
        CatJob *job = jobs + j;
        sb_counter_merge(&r->counters, &job->fetch_counts); sb_counter_merge(&r->counters, &job->parse_counts);
        int e = job->fetch_error ? job->fetch_error : job->parse_error;
        if (e) {
            ++r->counters.errors;
            if (e == EACCES || e == EPERM) ++r->counters.permission_errors;
            SBCompletion status = e == ENOTSUP || e == EINVAL ? SB_UNSUPPORTED : SB_FAILED;
            sb_failure(r, status, e, "catalog contract/search failed: %s; no fallback or truncated IDs used", strerror(e));
        }
        for (size_t k = 0; k < job->collector.count; ++k) {
            SBEntry entry = job->collector.entries[k];
            sb_collect(&all, cfg, entry, job->collector.names + entry.name_offset);
            all.entries[all.count - 1].reserved = entry.reserved;
        }
        sb_collector_destroy(&job->collector);
        sb_free(job->slots[0].data); sb_free(job->slots[1].data);
        pthread_cond_destroy(&job->ready); pthread_mutex_destroy(&job->lock);
    }
    sb_free(jobs);
    SBConfig retained = *cfg; retained.keep_manifest = true;
    sb_catalog_filter(&all, &retained, r);
    enrich_catalog(&all, cfg, r, fd, deadline);
    close(fd);
    r->scan_ns = sb_now_ns() - scan_start; uint64_t finish = sb_now_ns();
    sb_collect_finalize(&all, 1, cfg, r, inventory);
    r->finalize_ns = sb_now_ns() - finish;
#endif
}
