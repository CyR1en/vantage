#include "scanbench.h"
#include "attrs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "darwin_api.h"

int sb_allocated_at(int fd, const char *name, uint64_t id, uint64_t dev, uint64_t *size) {
#if defined(__APPLE__) || defined(SB_TEST_DARWIN)
    /* Canonical all-forks allocation, never silently substituted with st_blocks.
     * getattrlistat is available on the project's macOS 11+ deployment target. */
    struct attrlist a = {0}; a.bitmapcount = ATTR_BIT_MAP_COUNT;
    a.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID;
    a.fileattr = ATTR_FILE_ALLOCSIZE;
    unsigned char b[4096];
    int rc;
    do { rc = getattrlistat(fd, name, &a, b, sizeof(b), FSOPT_NOFOLLOW); } while (rc < 0 && errno == EINTR);
    if (rc) return -1;
    SBAttrSpec s = {.common = a.commonattr, .file = a.fileattr, .returned = true}; SBAttrRecord x;
    if (!sb_attr_decode(b, sizeof(b), s, SB_ALLOCATED, &x) || !x.has_allocation) { errno = ENOTSUP; return -1; }
    if (x.entry.object_id != id || x.entry.device != dev || x.entry.kind != 1) { errno = ESTALE; return -1; }
    *size = x.allocation; return 0;
#else
    (void)fd; (void)name; (void)id; (void)dev; (void)size;
    errno = ENOTSUP; return -1;
#endif
}
void sb_posix_directory(SBWorker *w, SBDirTask *task) {
    /* fdopendir takes ownership. The task relinquishes its fd after closedir. */
    int fd = task->fd;
    DIR *dir = fdopendir(fd);
    if (!dir) { sb_worker_error(w, errno); return; }
    const SBConfig *cfg = w->walk->config;
    uint64_t checkpoint = 0;
    for (;;) {
        errno = 0; ++w->counters.readdir_calls;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno == EINTR && !sb_walk_expired(w->walk)) continue;
            if (errno) sb_worker_error(w, errno);
            break;
        }
        if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) continue;
        if ((++checkpoint & 255) == 0 && sb_walk_expired(w->walk)) break;
        struct stat st; ++w->counters.stat_calls;
        int rc;
        do { rc = fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW); } while (rc < 0 && errno == EINTR);
        if (rc) { sb_worker_error(w, errno); continue; }
        SBEntry e = {0}; e.device = (uint64_t)st.st_dev; e.object_id = (uint64_t)st.st_ino;
        e.kind = sb_kind(st.st_mode); e.valid = SB_REQUIRED | SB_VALID_SIZE;
        e.name_length = (uint32_t)strlen(de->d_name);
        if (e.kind == 1 && cfg->task != SB_ENUMERATE) {
            if (cfg->size == SB_LOGICAL) {
                if (st.st_size < 0) { e.valid &= ~SB_VALID_SIZE; ++w->counters.unknown_attributes; }
                else e.size = (uint64_t)st.st_size;
            } else {
                ++w->counters.enrichment_calls;
                if (sb_allocated_at(fd, de->d_name, e.object_id, e.device, &e.size)) {
                    e.valid &= ~SB_VALID_SIZE; sb_worker_error(w, errno); ++w->counters.unknown_attributes;
                }
            }
        }
        sb_walk_entry(w, task, fd, e, de->d_name, false);
    }
    closedir(dir); task->fd = -1;
}
void sb_bulk_directory(SBWorker *w, SBDirTask *task) {
#if !defined(__APPLE__) && !defined(SB_TEST_DARWIN)
    (void)task;
    sb_walk_stop(w->walk, ENOTSUP, "getattrlistbulk requires macOS");
#else
    const SBConfig *cfg = w->walk->config;
    SBAttrSpec spec = sb_attr_spec(cfg, false);
    struct attrlist a = {0}; a.bitmapcount = ATTR_BIT_MAP_COUNT;
    a.commonattr = spec.common; a.dirattr = spec.dir; a.fileattr = spec.file;
    for (;;) {
        if (sb_walk_expired(w->walk)) return;
        uint64_t begin = cfg->diagnostic ? sb_now_ns() : 0;
        ++w->counters.bulk_calls;
        int n = getattrlistbulk(task->fd, &a, w->buffer, (size_t)cfg->buffer_size, cfg->pack_invalid ? FSOPT_PACK_INVAL_ATTRS : 0);
        if (cfg->diagnostic) w->counters.traversal_ns += sb_now_ns() - begin;
        if (n < 0) {
            int err = errno;
            if (err == EINTR) continue;
            if (err == ENOTSUP || err == EINVAL) {
                pthread_mutex_lock(&w->walk->lock);
                sb_failure(w->walk->result, SB_UNSUPPORTED, err, "bulk attribute set rejected by filesystem: %s", strerror(err));
                atomic_store(&w->walk->stop, true); pthread_cond_broadcast(&w->walk->ready);
                pthread_mutex_unlock(&w->walk->lock);
            } else sb_worker_error(w, err);
            return;
        }
        if (!n) return;
        ++w->counters.batches; w->counters.batch_entries += (uint64_t)n;
        if ((uint64_t)n > w->counters.max_batch) w->counters.max_batch = (uint64_t)n;
        size_t offset = 0;
        for (int j = 0; j < n; ++j) {
            SBAttrRecord x;
            if (!sb_attr_decode(w->buffer + offset, (size_t)cfg->buffer_size - offset, spec, cfg->size, &x)) {
                ++w->counters.malformed_records; sb_walk_stop(w->walk, EIO, "malformed bulk attribute record"); return;
            }
            offset += x.record_size;
            if (x.error) { sb_worker_error(w, (int)x.error); continue; }
            if (!x.name || !x.entry.name_length || !strcmp(x.name, ".") || !strcmp(x.name, "..")) continue;
            SBEntry e = x.entry;
            bool need_stat = (e.valid & (SB_VALID_ID | SB_VALID_DEVICE | SB_VALID_KIND)) != (SB_VALID_ID | SB_VALID_DEVICE | SB_VALID_KIND);
            /* Resolve boundary/firmlink target metadata just like fstatat. Bulk
             * reports the underlying mountpoint, not its mounted target. */
            bool boundary_lookup = (x.has_mountstatus && x.mountstatus) ||
                (x.has_flags && (x.flags & SB_F_FIRMLINK)) ||
                (e.kind == 2 && (!x.has_flags || !x.has_mountstatus));
            need_stat |= boundary_lookup;
            if (need_stat) {
                ++w->counters.enrichment_calls; ++w->counters.stat_calls;
                struct stat st;
                int rc;
                do { rc = fstatat(task->fd, x.name, &st, AT_SYMLINK_NOFOLLOW); } while (rc < 0 && errno == EINTR);
                if (rc) { sb_worker_error(w, errno); continue; }
                if (!(boundary_lookup && S_ISDIR(st.st_mode)) &&
                    (((e.valid & SB_VALID_ID) && e.object_id != (uint64_t)st.st_ino) ||
                     ((e.valid & SB_VALID_DEVICE) && e.device != (uint64_t)st.st_dev) ||
                     ((e.valid & SB_VALID_KIND) && e.kind != sb_kind(st.st_mode)))) {
                    sb_worker_error(w, ESTALE); continue;
                }
                /* Enrichment supplies a fresh identity; do not reuse a size
                 * observed before identity/type was established. */
                e.size = 0; e.valid &= ~SB_VALID_SIZE;
                e.device = (uint64_t)st.st_dev; e.object_id = (uint64_t)st.st_ino; e.kind = sb_kind(st.st_mode);
                e.valid |= SB_VALID_ID | SB_VALID_DEVICE | SB_VALID_KIND;
                if (cfg->task != SB_ENUMERATE && e.kind == 1 && cfg->size == SB_LOGICAL && st.st_size >= 0) { e.size = (uint64_t)st.st_size; e.valid |= SB_VALID_SIZE; }
            }
            if (e.kind == 1 && cfg->task != SB_ENUMERATE && !(e.valid & SB_VALID_SIZE)) {
                ++w->counters.enrichment_calls;
                if (cfg->size == SB_ALLOCATED) {
                    if (!sb_allocated_at(task->fd, x.name, e.object_id, e.device, &e.size)) e.valid |= SB_VALID_SIZE;
                    else sb_worker_error(w, errno);
                } else {
                    struct stat st; ++w->counters.stat_calls;
                    int rc;
                    do { rc = fstatat(task->fd, x.name, &st, AT_SYMLINK_NOFOLLOW); } while (rc < 0 && errno == EINTR);
                    if (rc) sb_worker_error(w, errno);
                    else if ((uint64_t)st.st_ino != e.object_id || (uint64_t)st.st_dev != e.device || !S_ISREG(st.st_mode))
                        sb_worker_error(w, ESTALE);
                    else if (st.st_size < 0) sb_worker_error(w, ENODATA);
                    else { e.size = (uint64_t)st.st_size; e.valid |= SB_VALID_SIZE; }
                }
                if (!(e.valid & SB_VALID_SIZE)) ++w->counters.unknown_attributes;
            }
            bool empty = cfg->skip_empty && cfg->consistency == SB_IMMUTABLE && e.kind == 2 && x.has_entrycount && x.entrycount == 0 && !need_stat;
            sb_walk_entry(w, task, task->fd, e, x.name, empty);
        }
        w->counters.metadata_bytes += offset;
        atomic_fetch_add(&w->walk->progress, (uint64_t)n);
        atomic_fetch_add_explicit(&sb_progress_entries, (uint64_t)n, memory_order_relaxed);
    }
#endif
}
