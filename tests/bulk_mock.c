#include "scanbench.h"
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
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
/* Original Linux/POSIX-backed test provider. These are NOT production fallbacks
 * and the timings of this mock are never exposed as filesystem benchmarks. */
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
static uint32_t child_count(int fd, const char *name) {
    int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(child >= 0); DIR *d = fdopendir(child); CHECK(d);
    uint32_t count = 0; struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) ++count;
    closedir(d); return count;
}
int getattrlistat(int fd, const char *name, void *attrs, void *out, size_t cap, unsigned long opts) {
    (void)opts; CHECK(cap >= 512);
    if (atomic_exchange(&interrupt_attribute, false)) { errno = EINTR; return -1; }
    struct stat st; if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    struct attrlist *a = attrs;
    SBAttrSpec s = {.common=a->commonattr, .dir=a->dirattr, .file=a->fileattr, .returned=true};
    SBEntry e = {.device=(uint64_t)st.st_dev, .object_id=(uint64_t)st.st_ino, .kind=sb_kind(st.st_mode), .size=(uint64_t)st.st_size};
    wire_encode(out, s, e, name, s.common, 0, s.file, 0, 0, 0);
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
    if (atomic_exchange(&interrupt_bulk, false)) { errno = EINTR; return -1; }
    if (scenario == 1) { errno = ENOTSUP; return -1; }
    if (scenario == 2) { memset(out, 0, cap); return 1; }
    struct attrlist *a = attrs;
    SBAttrSpec s = {.common=a->commonattr, .dir=a->dirattr, .file=a->fileattr,
        .returned=true, .pack_invalid=(opts & FSOPT_PACK_INVAL_ATTRS) != 0};
    pthread_mutex_lock(&lock);
    if (exhausted[fd]) { exhausted[fd] = false; pthread_mutex_unlock(&lock); return 0; }
    DIR *d = cursors[fd];
    if (!d) { int copy = dup(fd); CHECK(copy >= 0); d = cursors[fd] = fdopendir(copy); CHECK(d); }
    size_t offset = 0; int count = 0;
    while (cap - offset >= 512 && count < 3) {
        struct dirent *name = readdir(d);
        if (!name) { closedir(d); cursors[fd] = NULL; exhausted[fd] = count > 0; break; }
        if (!strcmp(name->d_name, ".") || !strcmp(name->d_name, "..")) continue;
        struct stat st; CHECK(!fstatat(fd, name->d_name, &st, AT_SYMLINK_NOFOLLOW));
        SBEntry e = {.device=(uint64_t)st.st_dev, .object_id=(uint64_t)st.st_ino,
            .kind=sb_kind(st.st_mode), .size=(uint64_t)st.st_size};
        uint32_t dc = e.kind == 2 ? s.dir : 0, fc = e.kind == 2 ? 0 : s.file;
        if (scenario == 3 && e.kind == 1) fc = 0; /* missing requested sizes */
        uint32_t common = s.common, flags = 0;
        if (scenario == 4 && e.kind == 1) {
            common &= ~SB_A_OBJTYPE;
            e.object_id += 100000; /* The name now resolves to another identity. */
        }
        if ((scenario == 5 && e.kind == 2) || (scenario == 6 && e.kind == 1)) {
            flags = SB_F_FIRMLINK;
            e.object_id += 100000; /* Underlying boundary vs resolved target. */
        }
        uint32_t children = e.kind == 2 && (s.dir & SB_D_ENTRYCOUNT) ? child_count(fd, name->d_name) : 0;
        offset += wire_encode((unsigned char *)out + offset, s, e, name->d_name, common, dc, fc, 0, flags, children);
        ++count;
    }
    pthread_mutex_unlock(&lock); return count;
}
static void clear_cursors(void) {
    for (size_t j = 0; j < 4096; ++j) { if (cursors[j]) { closedir(cursors[j]); cursors[j] = NULL; } exhausted[j] = false; }
}
int main(void) {
    char path[] = "/tmp/scanbench-bulk-XXXXXX"; CHECK(mkdtemp(path));
    int root = open(path, O_RDONLY | O_DIRECTORY); CHECK(root >= 0);
    CHECK(!mkdirat(root, "empty", 0700)); CHECK(!mkdirat(root, "dir", 0700));
    int file = openat(root, "file", O_CREAT | O_WRONLY, 0600); CHECK(file >= 0);
    CHECK(write(file, "abcdefg", 7) == 7); close(file);
    CHECK(!linkat(root, "file", root, "alias", 0)); CHECK(!symlinkat(".", root, "cycle"));
    int child = openat(root, "dir", O_RDONLY | O_DIRECTORY); CHECK(child >= 0);
    file = openat(child, "nested", O_CREAT | O_WRONLY, 0600); CHECK(file >= 0);
    CHECK(write(file, "123", 3) == 3); close(file); close(child);
    SBConfig base = {.method=SB_POSIX, .task=SB_TREE, .size=SB_LOGICAL, .workers=1,
        .keep_manifest=true, .buffer_size=4096, .queue_limit=512, .fd_limit=64, .consistency=SB_IMMUTABLE};
    snprintf(base.root, sizeof(base.root), "%s", path);
    SBResult gold = {.total_ns=sb_now_ns()}; SBInventory golden;
    sb_walk_run(&base, &gold, &golden); CHECK(gold.completion == SB_COMPLETE);
    for (unsigned sc = 0; sc < 7; ++sc) for (unsigned mode = 0; mode < 5; ++mode) {
        scenario = sc; SBConfig cfg = base; cfg.method = SB_BULK_PAR; cfg.workers = 4;
        cfg.pack_invalid = mode == 1; cfg.skip_empty = mode == 2;
        cfg.omit_objtype = mode == 3; cfg.extra_entrycount = cfg.extra_linkcount = cfg.extra_uuid = mode == 4;
        SBResult r = {.total_ns=sb_now_ns()}; SBInventory inv;
        sb_walk_run(&cfg, &r, &inv);
        if (sc == 1) CHECK(r.completion == SB_UNSUPPORTED);
        else if (sc == 2) CHECK(r.completion == SB_FAILED && r.counters.malformed_records > 0);
        else if (sc == 4 || sc == 6) CHECK(r.completion == SB_PARTIAL && r.error_code == ESTALE && r.counters.identity_races > 0 && r.files == 0);
        else {
            char why[256]; CHECK(r.completion == SB_COMPLETE);
            CHECK(sb_inventory_equal(&golden, &inv, why, sizeof(why)));
            if (cfg.skip_empty) CHECK(r.counters.empty_opens_avoided == (sc == 5 ? 0u : 1u));
            if (sc == 3 || cfg.omit_objtype) CHECK(r.counters.enrichment_calls > 0);
        }
        sb_inventory_destroy(&inv); clear_cursors();
    }
    scenario = 0;
    for (unsigned method = 0; method < 2; ++method) {
        SBConfig cfg = base; cfg.method = method ? SB_BULK : SB_POSIX; cfg.size = SB_ALLOCATED;
        SBResult r = {.total_ns=sb_now_ns()}; SBInventory inv;
        sb_walk_run(&cfg, &r, &inv); CHECK(r.completion == SB_COMPLETE && r.bytes == 34 && r.unique_bytes == 20);
        sb_inventory_destroy(&inv); clear_cursors();
    }
#ifdef __APPLE__
    expected_qos = QOS_CLASS_USER_INITIATED;
    CHECK(!pthread_set_qos_class_self_np(expected_qos, 0));
    SBConfig foreground = base; foreground.method = SB_BULK_PAR; foreground.workers = 4;
    SBResult foreground_result = {.total_ns=sb_now_ns()}; SBInventory foreground_inventory;
    sb_walk_run(&foreground, &foreground_result, &foreground_inventory);
    CHECK(foreground_result.completion == SB_COMPLETE);
    sb_inventory_destroy(&foreground_inventory); clear_cursors();
    expected_qos = QOS_CLASS_UNSPECIFIED;
    CHECK(!pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, 0));
    scenario = 7;
    snprintf(mounted_directory.f_mntonname, sizeof(mounted_directory.f_mntonname), "%s/dir", path);
    for (unsigned method = 0; method < 2; ++method) {
        SBConfig cfg = base; cfg.method = method ? SB_BULK_PAR : SB_POSIX_PAR; cfg.workers = 4;
        SBResult r = {.total_ns=sb_now_ns()}; SBInventory inv;
        sb_walk_run(&cfg, &r, &inv);
        CHECK(r.completion == SB_COMPLETE && r.graph_valid);
        CHECK(r.entries == 4 && r.bytes == 14 && r.counters.excluded_boundaries == 1);
        sb_inventory_destroy(&inv); clear_cursors();
        snprintf(cfg.root, sizeof(cfg.root), "%s/dir", path);
        r = (SBResult){.total_ns=sb_now_ns()};
        sb_walk_run(&cfg, &r, &inv);
        CHECK(r.completion == SB_COMPLETE && r.graph_valid && r.entries == 1 && r.bytes == 3);
        sb_inventory_destroy(&inv); clear_cursors();
    }
#endif
    scenario = 0;
    atomic_store(&interrupt_bulk, true);
    SBConfig interrupted = base; interrupted.method = SB_BULK_PAR; interrupted.workers = 4;
    SBResult retried = {.total_ns=sb_now_ns()}; SBInventory retried_inventory;
    sb_walk_run(&interrupted, &retried, &retried_inventory);
    CHECK(retried.completion == SB_COMPLETE && retried.counters.errors == 0);
    char retry_why[256];
    CHECK(sb_inventory_equal(&golden, &retried_inventory, retry_why, sizeof(retry_why)));
    sb_inventory_destroy(&retried_inventory); clear_cursors();
    scenario = 3;
    atomic_store(&interrupt_attribute, true);
    interrupted.size = SB_ALLOCATED;
    retried = (SBResult){.total_ns=sb_now_ns()};
    sb_walk_run(&interrupted, &retried, &retried_inventory);
    CHECK(retried.completion == SB_COMPLETE && retried.counters.errors == 0 && retried.bytes == 34);
    sb_inventory_destroy(&retried_inventory); clear_cursors();
    sb_inventory_destroy(&golden);
    child = openat(root, "dir", O_RDONLY | O_DIRECTORY); CHECK(child >= 0);
    CHECK(!unlinkat(child, "nested", 0)); close(child);
    CHECK(!unlinkat(root, "dir", AT_REMOVEDIR)); CHECK(!unlinkat(root, "empty", AT_REMOVEDIR));
    CHECK(!unlinkat(root, "file", 0)); CHECK(!unlinkat(root, "alias", 0)); CHECK(!unlinkat(root, "cycle", 0));
    close(root); CHECK(!rmdir(path));
    puts("bulk mock: production parallel traversal, variable/fixed packing, extra attributes, missing-size enrichment, omitted type, empty-directory elision, malformed/unsupported responses and allocation normalization passed");
    return 0;
}
