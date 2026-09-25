#include "scanbench.h"
#include "darwin_shim.h"
#include "attrs.h"
#include "wire.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static atomic_uint calls, starts, allocated_calls;
static atomic_bool busy_sent;
static unsigned scenario;
static uint64_t root_dev, root_id;
static SBEntry actual[11];
static const char *const paths[] = {"d", "d/a", "b", "alias", "cycle", "dangling", "fifo", "socket", "d/inner", "d/inner/leaf", "file-link"};
static const char *const names[] = {"d", "a", "b", "alias", "cycle", "dangling", "fifo", "socket", "inner", "leaf", "file-link"};
/* Each cursor returns one record per call. VREG deliberately includes native
 * APFS's ambiguous symlink/FIFO/socket records, independently checked by stat. */
int searchfs(const char *path, struct fssearchblock *s, unsigned long *matches,
             unsigned int script, unsigned int opts, struct searchstate *state) {
    (void)path; CHECK(script == 0x08000103u); atomic_fetch_add(&calls, 1);
    unsigned cursor = 0; memcpy(&cursor, state, sizeof(cursor));
    if (opts & SRCHFS_START) { cursor = 0; atomic_fetch_add(&starts, 1); }
    *matches = 0;
    if (scenario == 1) { errno = ENOTSUP; return -1; }
    if (scenario == 2 && cursor == 1 && !atomic_exchange(&busy_sent, true)) { errno = EBUSY; return -1; }
    if (scenario == 3) { errno = EBUSY; return -1; }
    if (scenario == 4) { memset(s->returnbuffer, 0, s->returnbuffersize); *matches = 1; return 0; }
    if (scenario == 5 && cursor == 0) { ++cursor; memcpy(state, &cursor, sizeof(cursor)); errno = EAGAIN; return -1; }
    unsigned record_index = cursor - (scenario == 5 ? 1u : 0u);
    uint64_t low = 0, high = UINT64_MAX;
    bool partition = s->searchattrs.commonattr == ATTR_CMN_FILEID;
    if (partition) { memcpy(&low, (unsigned char *)s->searchparams1 + 4, 8); memcpy(&high, (unsigned char *)s->searchparams2 + 4, 8); }
    else {
        CHECK(s->searchattrs.commonattr == ATTR_CMN_NAME && (opts & SRCHFS_MATCHPARTIALNAMES));
        CHECK(((unsigned char *)s->searchparams1)[12] == 0);
    }
    SBEntry entries[11]; memcpy(entries, actual, sizeof(entries));
    for (size_t j = 0; j < 11; ++j) if (entries[j].kind != 2) entries[j].kind = 1;
    if (scenario == 6) ++entries[1].object_id; /* name now identifies a different object */
    if (scenario == 9) {
        /* IDs deliberately above 32 bits and spanning all four partitions.
         * Their enrichment must reject invented identities, after joining all
         * search results. This retains independent partition-control coverage. */
        for (size_t j = 0; j < 11; ++j) if (entries[j].kind != 2)
            entries[j].object_id = ((uint64_t)(j % 4) << 62) | UINT64_C(0x100000011) | ((uint64_t)j << 8);
    }
    while (record_index < 11 && (entries[record_index].object_id < low || entries[record_index].object_id > high)) ++record_index;
    if (record_index >= 11) return 0;
    SBAttrSpec spec = {.common=s->returnattrs->commonattr, .dir=s->returnattrs->dirattr, .file=s->returnattrs->fileattr};
    CHECK(!(spec.common & SB_A_RETURNED));
    wire_encode(s->returnbuffer, spec, entries[record_index], names[record_index], spec.common, spec.dir, spec.file, 0, 0, 1);
    *matches = 1;
    cursor = record_index + 1 + (scenario == 5 ? 1u : 0u); memcpy(state, &cursor, sizeof(cursor));
    errno = EAGAIN; return -1;
}
/* The catalog-only test target does not link walk.c. Production uses its shared
 * ATTR_FILE_ALLOCSIZE helper; this deterministic provider keeps the test portable. */
int sb_allocated_at(int fd, const char *name, uint64_t id, uint64_t dev, uint64_t *size) {
    unsigned call = atomic_fetch_add(&allocated_calls, 1);
    if (scenario == 8) { errno = ENOTSUP; return -1; }
    if (scenario == 10 && !call) { struct timespec delay = {.tv_nsec=150000000}; nanosleep(&delay, NULL); }
    struct stat st;
    if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) return -1;
    if ((uint64_t)st.st_ino != id || (uint64_t)st.st_dev != dev || !S_ISREG(st.st_mode)) { errno = ESTALE; return -1; }
    *size = (uint64_t)st.st_size * 2; return 0;
}
static void run_case(const char *path, SBMethod method, unsigned sc, unsigned partitions, SBTask task, SBSize size) {
    scenario = sc; atomic_store(&calls, 0); atomic_store(&starts, 0); atomic_store(&busy_sent, false); atomic_store(&allocated_calls, 0);
    SBConfig cfg = {.method=method, .task=task, .size=size, .buffer_size=4096,
        .allow_volume_scan=true, .keep_manifest=sc != 11, .catalog_retries=2, .catalog_partitions=partitions,
        .fd_limit=4, .timeout_ns=sc == 10 ? UINT64_C(50000000) : UINT64_C(5000000000)};
    snprintf(cfg.root, sizeof(cfg.root), "%s", path);
    SBResult r = {.total_ns=sb_now_ns()}; SBInventory inv = {0};
    sb_catalog_run(&cfg, &r, &inv);
    if (sc == 1) CHECK(r.completion == SB_UNSUPPORTED);
    else if (sc == 3) CHECK(r.completion == SB_FAILED && r.counters.catalog_restarts == 2 && r.entries == 0);
    else if (sc == 4) CHECK(r.completion == SB_FAILED && r.counters.malformed_records == 1);
    else if (sc == 6 || sc == 7 || sc == 9) {
        CHECK(r.completion == SB_PARTIAL && r.entries < 11 && r.counters.identity_races > 0);
        CHECK(r.source_entries == 11);
        if (sc == 9) CHECK(atomic_load(&starts) == partitions);
    } else if (sc == 8) CHECK(r.completion == SB_PARTIAL && r.unknown_sizes == 4 && r.counters.enrichment_calls >= 11);
    else if (sc == 10) CHECK(r.completion == SB_FAILED && r.error_code == ETIMEDOUT && r.scan_ns >= UINT64_C(150000000));
    else {
        SBCollector gold; sb_collector_init(&gold, true);
        for (size_t j = 0; j < 11; ++j) {
            SBEntry e = actual[j]; e.valid = SB_REQUIRED | SB_VALID_SIZE;
            if (size == SB_ALLOCATED) e.size *= 2;
            sb_collect(&gold, &cfg, e, names[j]);
        }
        SBResult gr = {.root_device=root_dev, .root_id=root_id}; SBInventory golden;
        sb_collect_finalize(&gold, 1, &cfg, &gr, &golden);
        char why[256];
        CHECK(r.completion == SB_COMPLETE && r.entries == 11 && r.files == 4 && r.symlinks == 3 && r.other == 2);
        CHECK(sb_inventory_equal(&golden, &inv, why, sizeof(why)));
        CHECK(r.bytes == gr.bytes && r.unique_bytes == gr.unique_bytes && r.unique_files == gr.unique_files);
        CHECK(r.counters.enrichment_calls >= 11 && r.counters.stat_calls >= 11);
        if (size == SB_ALLOCATED && task != SB_ENUMERATE) CHECK(atomic_load(&allocated_calls) == 4);
        if (sc == 2) CHECK(r.counters.catalog_restarts == 1 && atomic_load(&starts) == 2);
        sb_inventory_destroy(&golden);
    }
    sb_inventory_destroy(&inv);
}
static void make_file(int root, const char *name, const char *data) {
    int fd = openat(root, name, O_WRONLY | O_CREAT | O_EXCL, 0600); CHECK(fd >= 0);
    size_t n = strlen(data); CHECK(write(fd, data, n) == (ssize_t)n); CHECK(!close(fd));
}
int main(void) {
    char root[] = "/tmp/scanbench-mock-XXXXXX"; CHECK(mkdtemp(root));
    int fd = open(root, O_RDONLY | O_DIRECTORY); CHECK(fd >= 0);
    CHECK(!mkdirat(fd, "d", 0700)); CHECK(!mkdirat(fd, "d/inner", 0700));
    make_file(fd, "d/a", "1234567"); make_file(fd, "b", "1234567890123"); make_file(fd, "d/inner/leaf", "123");
    CHECK(!linkat(fd, "d/a", fd, "alias", 0)); CHECK(!symlinkat(".", fd, "cycle"));
    CHECK(!symlinkat("absent", fd, "dangling")); CHECK(!symlinkat("b", fd, "file-link"));
    char fifo[sizeof(root) + 8]; CHECK(snprintf(fifo, sizeof(fifo), "%s/fifo", root) < (int)sizeof(fifo));
    CHECK(!mkfifo(fifo, 0600));
    int sock = socket(AF_UNIX, SOCK_STREAM, 0); CHECK(sock >= 0);
    struct sockaddr_un address = {.sun_family=AF_UNIX};
    CHECK(snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", root) < (int)sizeof(address.sun_path));
    CHECK(!bind(sock, (struct sockaddr *)&address, sizeof(address)));
    struct stat st; CHECK(!fstat(fd, &st)); root_dev = (uint64_t)st.st_dev; root_id = (uint64_t)st.st_ino;
    for (size_t j = 0; j < 11; ++j) {
        CHECK(!fstatat(fd, paths[j], &st, AT_SYMLINK_NOFOLLOW));
        actual[j] = (SBEntry){.device=(uint64_t)st.st_dev, .object_id=(uint64_t)st.st_ino,
            .parent_id=j == 1 || j == 8 ? actual[0].object_id : j == 9 ? actual[8].object_id : root_id,
            .kind=sb_kind(st.st_mode), .size=(uint64_t)st.st_size, .name_length=(uint32_t)strlen(names[j])};
    }
    for (unsigned sc = 0; sc <= 6; ++sc) {
        run_case(root, SB_CATALOG, sc, 1, SB_TREE, SB_LOGICAL);
        run_case(root, SB_CATALOG_PIPE, sc, 1, SB_TREE, SB_LOGICAL);
    }
    for (unsigned task = SB_ENUMERATE; task <= SB_TREE; ++task) for (unsigned size = SB_LOGICAL; size <= SB_ALLOCATED; ++size)
        run_case(root, SB_CATALOG_PIPE, 0, 1, (SBTask)task, (SBSize)size);
    CHECK(!renameat(fd, "d", fd, "moved")); CHECK(!symlinkat("moved", fd, "d"));
    run_case(root, SB_CATALOG_PIPE, 7, 1, SB_TREE, SB_LOGICAL);
    CHECK(!unlinkat(fd, "d", 0)); CHECK(!renameat(fd, "moved", fd, "d"));
    run_case(root, SB_CATALOG, 8, 1, SB_TREE, SB_ALLOCATED);
    run_case(root, SB_CATALOG_PARTITION, 0, 2, SB_TREE, SB_LOGICAL);
    run_case(root, SB_CATALOG_PARTITION, 0, 4, SB_TREE, SB_LOGICAL);
    run_case(root, SB_CATALOG_PARTITION, 9, 2, SB_TREE, SB_LOGICAL);
    run_case(root, SB_CATALOG_PARTITION, 9, 4, SB_TREE, SB_LOGICAL);
    run_case(root, SB_CATALOG_PIPE, 10, 1, SB_TREE, SB_ALLOCATED);
    struct rlimit saved, limited; CHECK(!getrlimit(RLIMIT_NOFILE, &saved)); limited = saved;
    if (limited.rlim_cur > 32) limited.rlim_cur = 32;
    CHECK(!setrlimit(RLIMIT_NOFILE, &limited));
    run_case(root, SB_CATALOG_PIPE, 11, 1, SB_USAGE, SB_ALLOCATED);
    CHECK(!setrlimit(RLIMIT_NOFILE, &saved));
    close(sock);
    for (size_t j = 11; j; --j) CHECK(!unlinkat(fd, paths[j - 1], actual[j - 1].kind == 2 ? AT_REMOVEDIR : 0));
    close(fd); CHECK(!rmdir(root));
    puts("catalog mock: native-style type enrichment, exact manifests, all tasks/sizes, cross-parent hardlinks, no-follow identities, allocation errors, enrichment deadline, continuation, restart, malformed data, pipeline cancellation and disjoint 64-bit partitions passed");
    return 0;
}
