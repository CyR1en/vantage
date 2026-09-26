#include "scan.h"
#include "attrs.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "darwin_api.h"

static int allocated_at(int fd, const char *name, uint64_t id, uint64_t dev, uint64_t *size) {
#if defined(__APPLE__) || defined(VS_TEST_DARWIN)
    /* Canonical all-forks allocation, never silently substituted with st_blocks.
     * getattrlistat is available on the project's macOS 11+ deployment target. */
    struct attrlist a = {0};
    a.bitmapcount = ATTR_BIT_MAP_COUNT;
    a.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE |
                   ATTR_CMN_FILEID;
    a.fileattr = ATTR_FILE_ALLOCSIZE;
    unsigned char b[4096];
    int rc;
    do {
        rc = getattrlistat(fd, name, &a, b, sizeof(b), FSOPT_NOFOLLOW);
    } while (rc < 0 && errno == EINTR);
    if (rc) {
        return -1;
    }
    VSAttrSpec s = {.common = a.commonattr, .file = a.fileattr};
    VSAttrRecord x;
    if (!vs_attr_decode(b, sizeof(b), s, VS_ALLOCATED, &x) || !x.has_allocation) {
        errno = ENOTSUP;
        return -1;
    }
    if (x.entry.object_id != id || x.entry.device != dev || x.entry.kind != 1) {
        errno = ESTALE;
        return -1;
    }
    *size = x.allocation;
    return 0;
#else
    (void)fd;
    (void)name;
    (void)id;
    (void)dev;
    (void)size;
    errno = ENOTSUP;
    return -1;
#endif
}

void vs_posix_directory(VSWorker *w, VSDirTask *task) {
    /* fdopendir takes ownership. The task relinquishes its fd after closedir. */
    int fd = task->fd;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        vs_worker_error(w, errno);
        return;
    }
    const VSConfig *cfg = w->walk->config;
    uint64_t checkpoint = 0;
    for (;;) {
        errno = 0;

        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno == EINTR && !vs_walk_expired(w->walk)) {
                continue;
            }
            if (errno) {
                vs_worker_error(w, errno);
            }
            break;
        }
        if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) {
            continue;
        }
        if ((++checkpoint & 255) == 0 && vs_walk_expired(w->walk)) {
            break;
        }
        struct stat st;

        int rc;
        do {
            rc = fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW);
        } while (rc < 0 && errno == EINTR);
        if (rc) {
            vs_worker_error(w, errno);
            continue;
        }
        VSEntry e = {0};
        e.device = (uint64_t)st.st_dev;
        e.object_id = (uint64_t)st.st_ino;
        e.kind = vs_kind(st.st_mode);
        e.valid = VS_REQUIRED | VS_VALID_SIZE;
        e.name_length = (uint32_t)strlen(de->d_name);
        if (e.kind == 1) {
            if (cfg->size == VS_LOGICAL) {
                if (st.st_size < 0) {
                    e.valid &= ~VS_VALID_SIZE;
                    ++w->counters.unknown_attributes;
                } else {
                    e.size = (uint64_t)st.st_size;
                }
            } else {
                ++w->counters.enrichment_calls;
                if (allocated_at(fd, de->d_name, e.object_id, e.device, &e.size)) {
                    e.valid &= ~VS_VALID_SIZE;
                    vs_worker_error(w, errno);
                    ++w->counters.unknown_attributes;
                }
            }
        }
        vs_walk_entry(w, task, fd, e, de->d_name);
    }
    closedir(dir);
    task->fd = -1;
}

void vs_bulk_directory(VSWorker *w, VSDirTask *task) {
#if !defined(__APPLE__) && !defined(VS_TEST_DARWIN)
    (void)task;
    vs_walk_stop(w->walk, ENOTSUP, "getattrlistbulk requires macOS");
#else
    const VSConfig *cfg = w->walk->config;
    VSAttrSpec spec = vs_attr_spec(cfg->size);
    struct attrlist a = {0};
    a.bitmapcount = ATTR_BIT_MAP_COUNT;
    a.commonattr = spec.common;
    a.dirattr = spec.dir;
    a.fileattr = spec.file;
    for (;;) {
        if (vs_walk_expired(w->walk)) {
            return;
        }

        int n = getattrlistbulk(task->fd, &a, w->buffer, (size_t)cfg->buffer_size, 0);
        if (n < 0) {
            int err = errno;
            if (err == EINTR) {
                continue;
            }
            if (err == ENOTSUP || err == EINVAL) {
                pthread_mutex_lock(&w->walk->lock);
                vs_failure(w->walk->result, VS_UNSUPPORTED, err,
                           "bulk attribute set rejected by filesystem: %s", strerror(err));
                atomic_store(&w->walk->stop, true);
                pthread_cond_broadcast(&w->walk->ready);
                pthread_mutex_unlock(&w->walk->lock);
            } else {
                vs_worker_error(w, err);
            }
            return;
        }
        if (!n) {
            return;
        }

        size_t offset = 0;
        for (int j = 0; j < n; ++j) {
            VSAttrRecord x;
            if (!vs_attr_decode(w->buffer + offset, (size_t)cfg->buffer_size - offset, spec,
                                cfg->size, &x)) {
                ++w->counters.malformed_records;
                vs_walk_stop(w->walk, EIO, "malformed bulk attribute record");
                return;
            }
            offset += x.record_size;
            if (x.error) {
                vs_worker_error(w, (int)x.error);
                continue;
            }
            if (!x.name || !x.entry.name_length || !strcmp(x.name, ".") || !strcmp(x.name, "..")) {
                continue;
            }
            VSEntry e = x.entry;
            bool need_stat = (e.valid & (VS_VALID_ID | VS_VALID_DEVICE | VS_VALID_KIND)) !=
                             (VS_VALID_ID | VS_VALID_DEVICE | VS_VALID_KIND);
            /* Resolve boundary/firmlink target metadata just like fstatat. Bulk
             * reports the underlying mountpoint, not its mounted target. */
            bool boundary_lookup = (x.has_mountstatus && x.mountstatus) ||
                                   (x.has_flags && (x.flags & VS_F_FIRMLINK)) ||
                                   (e.kind == 2 && (!x.has_flags || !x.has_mountstatus));
            need_stat |= boundary_lookup;
            if (need_stat) {
                ++w->counters.enrichment_calls;

                struct stat st;
                int rc;
                do {
                    rc = fstatat(task->fd, x.name, &st, AT_SYMLINK_NOFOLLOW);
                } while (rc < 0 && errno == EINTR);
                if (rc) {
                    vs_worker_error(w, errno);
                    continue;
                }
                if (!(boundary_lookup && S_ISDIR(st.st_mode)) &&
                    (((e.valid & VS_VALID_ID) && e.object_id != (uint64_t)st.st_ino) ||
                     ((e.valid & VS_VALID_DEVICE) && e.device != (uint64_t)st.st_dev) ||
                     ((e.valid & VS_VALID_KIND) && e.kind != vs_kind(st.st_mode)))) {
                    vs_worker_error(w, ESTALE);
                    continue;
                }
                /* Enrichment supplies a fresh identity; do not reuse a size
                 * observed before identity/type was established. */
                e.size = 0;
                e.valid &= ~VS_VALID_SIZE;
                e.device = (uint64_t)st.st_dev;
                e.object_id = (uint64_t)st.st_ino;
                e.kind = vs_kind(st.st_mode);
                e.valid |= VS_VALID_ID | VS_VALID_DEVICE | VS_VALID_KIND;
                if (e.kind == 1 && cfg->size == VS_LOGICAL && st.st_size >= 0) {
                    e.size = (uint64_t)st.st_size;
                    e.valid |= VS_VALID_SIZE;
                }
            }
            if (e.kind == 1 && !(e.valid & VS_VALID_SIZE)) {
                ++w->counters.enrichment_calls;
                if (cfg->size == VS_ALLOCATED) {
                    if (!allocated_at(task->fd, x.name, e.object_id, e.device, &e.size)) {
                        e.valid |= VS_VALID_SIZE;
                    } else {
                        vs_worker_error(w, errno);
                    }
                } else {
                    struct stat st;

                    int rc;
                    do {
                        rc = fstatat(task->fd, x.name, &st, AT_SYMLINK_NOFOLLOW);
                    } while (rc < 0 && errno == EINTR);
                    if (rc) {
                        vs_worker_error(w, errno);
                    } else if ((uint64_t)st.st_ino != e.object_id ||
                               (uint64_t)st.st_dev != e.device || !S_ISREG(st.st_mode)) {
                        vs_worker_error(w, ESTALE);
                    } else if (st.st_size < 0) {
                        vs_worker_error(w, ENODATA);
                    } else {
                        e.size = (uint64_t)st.st_size;
                        e.valid |= VS_VALID_SIZE;
                    }
                }
                if (!(e.valid & VS_VALID_SIZE)) {
                    ++w->counters.unknown_attributes;
                }
            }
            vs_walk_entry(w, task, task->fd, e, x.name);
        }
        atomic_fetch_add_explicit(&vs_progress_entries, (uint64_t)n, memory_order_relaxed);
    }
#endif
}
