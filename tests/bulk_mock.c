#include "scan.h"
#include "attrs.h"
#include "darwin_shim.h"
#include "wire.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);                  \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
/* POSIX-backed provider exercises native bulk failure paths on every platform. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static DIR *cursors[4096];
static bool exhausted[4096];
static unsigned scenario;
static atomic_bool interrupt_bulk;
static atomic_bool interrupt_attribute;
#ifdef __APPLE__
static struct statfs mounted_directory;
static qos_class_t expected_qos;

int getmntinfo(struct statfs **mounts, int flags) {
    (void)flags;
    *mounts = &mounted_directory;
    return scenario == 7 ? 1 : 0;
}
#endif
int getattrlistat(int fd, const char *name, void *attrs, void *out, size_t cap,
                  unsigned long opts) {
    (void)opts;
    CHECK(cap >= 512);
    if (atomic_exchange(&interrupt_attribute, false)) {
        errno = EINTR;
        return -1;
    }
    struct stat st;
    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
        return -1;
    }
    struct attrlist *a = attrs;
    VSAttrSpec s = {.common = a->commonattr, .dir = a->dirattr, .file = a->fileattr};
    VSEntry e = {.device = (uint64_t)st.st_dev,
                 .object_id = (uint64_t)st.st_ino,
                 .kind = vs_kind(st.st_mode),
                 .size = (uint64_t)st.st_size};
    wire_encode(out, s, e, name, s.common, 0, s.file, 0, 0);
    return 0;
}

int getattrlistbulk(int fd, void *attrs, void *out, size_t cap, uint64_t opts) {
    CHECK(fd >= 0 && fd < 4096 && cap >= 1024);
#ifdef __APPLE__
    if (expected_qos) {
        qos_class_t actual_qos;
        CHECK(!pthread_get_qos_class_np(pthread_self(), &actual_qos, NULL));
        CHECK(actual_qos == expected_qos);
    }
#endif
    if (atomic_exchange(&interrupt_bulk, false)) {
        errno = EINTR;
        return -1;
    }
    if (scenario == 1) {
        errno = ENOTSUP;
        return -1;
    }
    if (scenario == 2) {
        memset(out, 0, cap);
        return 1;
    }
    struct attrlist *a = attrs;
    VSAttrSpec s = {.common = a->commonattr,
                    .dir = a->dirattr,
                    .file = a->fileattr,
                    .pack_invalid = (opts & FSOPT_PACK_INVAL_ATTRS) != 0};
    pthread_mutex_lock(&lock);
    if (exhausted[fd]) {
        exhausted[fd] = false;
        pthread_mutex_unlock(&lock);
        return 0;
    }
    DIR *d = cursors[fd];
    if (!d) {
        int copy = dup(fd);
        CHECK(copy >= 0);
        d = cursors[fd] = fdopendir(copy);
        CHECK(d);
    }
    size_t offset = 0;
    int count = 0;
    while (cap - offset >= 512 && count < 3) {
        struct dirent *name = readdir(d);
        if (!name) {
            closedir(d);
            cursors[fd] = NULL;
            exhausted[fd] = count > 0;
            break;
        }
        if (!strcmp(name->d_name, ".") || !strcmp(name->d_name, "..")) {
            continue;
        }
        struct stat st;
        CHECK(!fstatat(fd, name->d_name, &st, AT_SYMLINK_NOFOLLOW));
        VSEntry e = {.device = (uint64_t)st.st_dev,
                     .object_id = (uint64_t)st.st_ino,
                     .kind = vs_kind(st.st_mode),
                     .size = (uint64_t)st.st_size};
        uint32_t dc = e.kind == 2 ? s.dir : 0, fc = e.kind == 2 ? 0 : s.file;
        if (scenario == 3 && e.kind == 1) {
            fc = 0; /* missing requested sizes */
        }
        uint32_t common = s.common, flags = 0;
        if (scenario == 8) {
            common &= ~VS_A_OBJTYPE;
        }
        if (scenario == 4 && e.kind == 1) {
            common &= ~VS_A_OBJTYPE;
            e.object_id += 100000; /* The name now resolves to another identity. */
        }
        if ((scenario == 5 && e.kind == 2) || (scenario == 6 && e.kind == 1)) {
            flags = VS_F_FIRMLINK;
            e.object_id += 100000; /* Underlying boundary vs resolved target. */
        }
        uint32_t error = scenario == 9 ? EACCES : 0;
        if (error) {
            common = VS_A_RETURNED | VS_A_ERROR | VS_A_NAME;
            dc = fc = 0;
        }
        offset += wire_encode((unsigned char *)out + offset, s, e, name->d_name, common, dc, fc,
                              error, flags);
        ++count;
    }
    pthread_mutex_unlock(&lock);
    return count;
}

static void clear_cursors(void) {
    for (size_t j = 0; j < 4096; ++j) {
        if (cursors[j]) {
            closedir(cursors[j]);
            cursors[j] = NULL;
        }
        exhausted[j] = false;
    }
}

static void check_inventory(const VSInventory *expected, const VSInventory *actual) {
    CHECK(expected->root_device == actual->root_device && expected->root_id == actual->root_id &&
          expected->count == actual->count);
    bool *matched = vs_calloc(actual->count, sizeof(*matched));
    for (size_t j = 0; j < expected->count; ++j) {
        const VSEntry *a = expected->entries + j;
        bool found = false;
        for (size_t k = 0; k < actual->count; ++k) {
            const VSEntry *b = actual->entries + k;
            if (!matched[k] && a->device == b->device && a->object_id == b->object_id &&
                a->parent_id == b->parent_id && a->kind == b->kind && a->valid == b->valid &&
                a->size == b->size && a->subtree_bytes == b->subtree_bytes &&
                a->subtree_unknown == b->subtree_unknown && a->name_length == b->name_length &&
                !memcmp(expected->names + a->name_offset, actual->names + b->name_offset,
                        a->name_length)) {
                matched[k] = true;
                found = true;
                break;
            }
        }
        CHECK(found);
    }
    vs_free(matched);
}

int main(void) {
    char path[] = "/tmp/vantage-bulk-XXXXXX";
    CHECK(mkdtemp(path));
    int root = open(path, O_RDONLY | O_DIRECTORY);
    CHECK(root >= 0);
    CHECK(!mkdirat(root, "empty", 0700));
    CHECK(!mkdirat(root, "dir", 0700));
    int file = openat(root, "file", O_CREAT | O_WRONLY, 0600);
    CHECK(file >= 0);
    CHECK(write(file, "abcdefg", 7) == 7);
    close(file);
    CHECK(!linkat(root, "file", root, "alias", 0));
    CHECK(!symlinkat(".", root, "cycle"));
    int child = openat(root, "dir", O_RDONLY | O_DIRECTORY);
    CHECK(child >= 0);
    file = openat(child, "nested", O_CREAT | O_WRONLY, 0600);
    CHECK(file >= 0);
    CHECK(write(file, "123", 3) == 3);
    close(file);
    close(child);
    VSConfig base = {.method = VS_POSIX,
                     .size = VS_LOGICAL,
                     .workers = 1,
                     .buffer_size = 4096,
                     .queue_limit = 512,
                     .fd_limit = 64};
    snprintf(base.root, sizeof(base.root), "%s", path);
    VSResult gold = {.total_ns = vs_now_ns()};
    VSInventory golden;
    vs_walk_run(&base, &gold, &golden);
    CHECK(gold.completion == VS_COMPLETE);
    const unsigned scenarios[] = {0, 1, 2, 3, 4, 5, 6, 8, 9};
    for (size_t j = 0; j < sizeof(scenarios) / sizeof(*scenarios); ++j) {
        for (unsigned workers = 1; workers <= 4; workers += 3) {
            unsigned sc = scenario = scenarios[j];
            VSConfig cfg = base;
            cfg.method = VS_BULK;
            cfg.workers = workers;
            VSResult r = {.total_ns = vs_now_ns()};
            VSInventory inv;
            vs_walk_run(&cfg, &r, &inv);
            if (sc == 1) {
                CHECK(r.completion == VS_UNSUPPORTED && !r.entries);
            } else if (sc == 2) {
                CHECK(r.completion == VS_FAILED && r.counters.malformed_records > 0);
            } else if (sc == 4 || sc == 6) {
                CHECK(r.completion == VS_PARTIAL && r.error_code == ESTALE &&
                      r.counters.identity_races > 0 && r.files == 0);
            } else if (sc == 9) {
                CHECK(r.completion == VS_PARTIAL && r.counters.permission_errors == 5 &&
                      !r.counters.malformed_records && !r.files);
            } else {
                CHECK(r.completion == VS_COMPLETE);
                check_inventory(&golden, &inv);
                if (sc == 3 || sc == 8) {
                    CHECK(r.counters.enrichment_calls > 0);
                }
            }
            vs_inventory_destroy(&inv);
            clear_cursors();
        }
    }
    scenario = 0;
    for (unsigned method = 0; method < 2; ++method) {
        VSConfig cfg = base;
        cfg.method = method ? VS_BULK : VS_POSIX;
        cfg.size = VS_ALLOCATED;
        VSResult r = {.total_ns = vs_now_ns()};
        VSInventory inv;
        vs_walk_run(&cfg, &r, &inv);
        CHECK(r.completion == VS_COMPLETE && r.bytes == 34 && r.unique_bytes == 20);
        vs_inventory_destroy(&inv);
        clear_cursors();
    }
#ifdef __APPLE__
    expected_qos = QOS_CLASS_USER_INITIATED;
    CHECK(!pthread_set_qos_class_self_np(expected_qos, 0));
    VSConfig foreground = base;
    foreground.method = VS_BULK;
    foreground.workers = 4;
    VSResult foreground_result = {.total_ns = vs_now_ns()};
    VSInventory foreground_inventory;
    vs_walk_run(&foreground, &foreground_result, &foreground_inventory);
    CHECK(foreground_result.completion == VS_COMPLETE);
    vs_inventory_destroy(&foreground_inventory);
    clear_cursors();
    expected_qos = QOS_CLASS_UNSPECIFIED;
    CHECK(!pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0));
    scenario = 7;
    snprintf(mounted_directory.f_mntonname, sizeof(mounted_directory.f_mntonname), "%s/dir", path);
    for (unsigned method = 0; method < 2; ++method) {
        VSConfig cfg = base;
        cfg.method = method ? VS_BULK : VS_POSIX;
        cfg.workers = 4;
        VSResult r = {.total_ns = vs_now_ns()};
        VSInventory inv;
        vs_walk_run(&cfg, &r, &inv);
        CHECK(r.completion == VS_COMPLETE && r.graph_valid);
        CHECK(r.entries == 4 && r.bytes == 14 && r.counters.excluded_boundaries == 1);
        vs_inventory_destroy(&inv);
        clear_cursors();
        snprintf(cfg.root, sizeof(cfg.root), "%s/dir", path);
        r = (VSResult){.total_ns = vs_now_ns()};
        vs_walk_run(&cfg, &r, &inv);
        CHECK(r.completion == VS_COMPLETE && r.graph_valid && r.entries == 1 && r.bytes == 3);
        vs_inventory_destroy(&inv);
        clear_cursors();
    }
#endif
    scenario = 0;
    for (unsigned method = 0; method < 2; ++method) {
        VSConfig limited = base;
        limited.method = method ? VS_BULK : VS_POSIX;
        limited.queue_limit = 1;
        VSResult r = {.total_ns = vs_now_ns()};
        VSInventory inv;
        vs_walk_run(&limited, &r, &inv);
        CHECK(r.completion == VS_FAILED && r.error_code == ENOBUFS);
        vs_inventory_destroy(&inv);
        clear_cursors();
        limited.queue_limit = base.queue_limit;
        limited.timeout_ns = 1;
        r = (VSResult){.total_ns = vs_now_ns()};
        vs_walk_run(&limited, &r, &inv);
        CHECK(r.completion == VS_FAILED && r.error_code == ETIMEDOUT);
        vs_inventory_destroy(&inv);
        clear_cursors();
    }
    scenario = 0;
    atomic_store(&interrupt_bulk, true);
    VSConfig interrupted = base;
    interrupted.method = VS_BULK;
    interrupted.workers = 4;
    VSResult retried = {.total_ns = vs_now_ns()};
    VSInventory retried_inventory;
    vs_walk_run(&interrupted, &retried, &retried_inventory);
    CHECK(retried.completion == VS_COMPLETE && retried.counters.errors == 0);
    check_inventory(&golden, &retried_inventory);
    vs_inventory_destroy(&retried_inventory);
    clear_cursors();
    scenario = 3;
    atomic_store(&interrupt_attribute, true);
    interrupted.size = VS_ALLOCATED;
    retried = (VSResult){.total_ns = vs_now_ns()};
    vs_walk_run(&interrupted, &retried, &retried_inventory);
    CHECK(retried.completion == VS_COMPLETE && retried.counters.errors == 0 && retried.bytes == 34);
    vs_inventory_destroy(&retried_inventory);
    clear_cursors();
    vs_inventory_destroy(&golden);
    child = openat(root, "dir", O_RDONLY | O_DIRECTORY);
    CHECK(child >= 0);
    CHECK(!unlinkat(child, "nested", 0));
    close(child);
    CHECK(!unlinkat(root, "dir", AT_REMOVEDIR));
    CHECK(!unlinkat(root, "empty", AT_REMOVEDIR));
    CHECK(!unlinkat(root, "file", 0));
    CHECK(!unlinkat(root, "alias", 0));
    CHECK(!unlinkat(root, "cycle", 0));
    close(root);
    CHECK(!rmdir(path));
    puts(
        "bulk mock: serial/parallel traversal, metadata enrichment, identity races, malformed/unsupported responses, permission errors, bounded resources, EINTR and allocation normalization passed");
    return 0;
}
