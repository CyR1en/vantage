#include "vantage.h"
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

typedef struct {
    SBConfig scan;
    const char *target;
    size_t top;
    unsigned depth;
    int interactive;
    bool files, json, automatic, export;
} Options;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    const SBConfig *config;
    uint64_t started;
    bool done, running, machine;
} Progress;

static volatile sig_atomic_t progress_visible;

static void scan_signal(int sig) {
    if (progress_visible) {
        ssize_t ignored = write(STDERR_FILENO, "\n", 1);
        (void)ignored;
    }
    _exit(128 + sig);
}

static void help(void) {
    puts("vantage " VANTAGE_VERSION " - find your largest directories and files\n"
         "\nUsage: vantage [DIRECTORY] [OPTIONS]\n"
         "\nScans once, then opens a terminal browser. Default directory: current.\n"
         "When piped, prints a ranked report instead. No files are modified.\n"
         "\n  --top N, -n N          Plain/JSON rows per directory / files (default 20)\n"
         "  --depth N              Plain/JSON tree levels, 1..8 (default 1)\n"
         "  --files, -f            Start with the largest files across the scan\n"
         "  --no-interactive       Print size bars and rankings, then exit\n"
         "  --interactive          Require an interactive terminal\n"
         "  --json                 Machine-readable rankings with lossless path_hex\n"
         "  --export               Complete binary tree for graphical front ends\n"
         "  --allocated, -a        Use filesystem allocation (macOS, all forks)\n"
         "  --size logical|allocated  Default logical: regular-file data-fork length\n"
         "  --workers N, -j N      Parallel workers, 1..64 (Mac: up to 16; other: 4)\n"
         "  --method auto|bulk|posix  Auto: macOS bulk, POSIX elsewhere\n"
         "  --timeout SECONDS      Scan deadline, 0 disables (default 300)\n"
         "  --memory-limit BYTES   Managed-allocation cap (default 1GiB)\n"
         "  --fd-limit N           Directory-descriptor budget, >=4 (Mac: 8192; other: 256)\n"
         "  --help, -h             Show this help\n"
         "  --version              Show version\n"
         "\nBrowser: arrows/j/k move; Enter/right/l open; left/h/Backspace go up;\n"
         "         f toggles largest files; g returns to root; q quits.\n"
         "\nSymlinks are not followed; scans stay on one filesystem. Hard-link names\n"
         "count separately in directory bars; unique file bytes are also shown.\n"
         "Allocation is not reclaimable/exclusive space. Live scans are best effort;\n"
         "partial results are labeled as lower bounds and exit nonzero.\n"
         "Bulk uses 64 KiB batches. Auto retries POSIX only if bulk is unsupported.\n"
         "Exit: 0 complete, 1 partial/failed, 2 unsupported, 64 usage, 71 resource error.");
}

static _Noreturn void usage(const char *reason) {
    fprintf(stderr, "vantage: %s\nUse --help for usage.\n", reason);
    exit(64);
}

static uint64_t number(const char *text, bool bytes) {
    if (!text || *text < '0' || *text > '9') usage("expected a nonnegative integer");
    errno = 0;
    char *end;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno) usage("integer is too large");
    uint64_t scale = 1;
    if (*end) {
        if (!bytes) usage("unexpected suffix on integer");
        if (!strcmp(end, "KiB") || !strcmp(end, "K")) scale = 1024;
        else if (!strcmp(end, "MiB") || !strcmp(end, "M")) scale = UINT64_C(1024) * 1024;
        else if (!strcmp(end, "GiB") || !strcmp(end, "G")) scale = UINT64_C(1024) * 1024 * 1024;
        else if (strcmp(end, "B")) usage("size suffix must be B, KiB, MiB, or GiB");
    }
    if (parsed > UINT64_MAX / scale) usage("scaled integer is too large");
    return (uint64_t)parsed * scale;
}

static void parse(Options *o, int argc, char **argv) {
    memset(o, 0, sizeof(*o));
    o->automatic = true; o->interactive = -1; o->top = 20; o->depth = 1;
    o->scan = (SBConfig){.method = SB_POSIX_PAR, .task = SB_TREE, .size = SB_LOGICAL,
        .order = SB_DFS, .consistency = SB_LIVE, .cache = SB_CACHE_FIRST,
        .workers = 4, .buffer_size = 65536, .queue_limit = 262144, .fd_limit = 256,
        .memory_limit = UINT64_C(1024) * 1024 * 1024, .timeout_ns = UINT64_C(300000000000),
        .keep_manifest = true, .catalog_partitions = 1, .seed = 42};
#ifdef __APPLE__
    o->scan.method = SB_BULK_PAR;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    o->scan.workers = cpus > 0 && cpus < 8 ? (uint32_t)cpus * 2 : 16;
    o->scan.fd_limit = 8192;
#endif
    for (int j = 1; j < argc; ++j) {
        const char *arg = argv[j];
        if (!strcmp(arg, "--")) {
            if (++j != argc - 1 || o->target) usage("expected one directory after --");
            o->target = argv[j]; break;
        }
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { help(); exit(0); }
        if (!strcmp(arg, "--version")) { puts("vantage " VANTAGE_VERSION); exit(0); }
        if (!strcmp(arg, "--files") || !strcmp(arg, "-f")) { o->files = true; continue; }
        if (!strcmp(arg, "--allocated") || !strcmp(arg, "-a")) { o->scan.size = SB_ALLOCATED; continue; }
        if (!strcmp(arg, "--json")) { o->json = true; continue; }
        if (!strcmp(arg, "--export")) { o->export = true; continue; }
        if (!strcmp(arg, "--interactive")) { o->interactive = 1; continue; }
        if (!strcmp(arg, "--no-interactive")) { o->interactive = 0; continue; }
        if (*arg != '-' || !strcmp(arg, "-")) {
            if (o->target) usage("supply only one directory");
            o->target = arg; continue;
        }
        char key[64];
        const char *equal = strchr(arg, '=');
        size_t length = equal ? (size_t)(equal - arg) : strlen(arg);
        if (length >= sizeof(key)) usage("unknown option");
        memcpy(key, arg, length); key[length] = 0;
        const char *value = equal ? equal + 1 : ++j < argc ? argv[j] : NULL;
        if (!value || !*value || !strncmp(value, "--", 2)) usage("missing option value");
        if (!strcmp(key, "--top") || !strcmp(key, "-n")) {
            uint64_t n = number(value, false);
            if (!n || n > 10000) usage("--top must be 1..10000");
            o->top = (size_t)n;
        } else if (!strcmp(key, "--depth")) {
            uint64_t n = number(value, false);
            if (!n || n > 8) usage("--depth must be 1..8");
            o->depth = (unsigned)n;
        } else if (!strcmp(key, "--workers") || !strcmp(key, "-j")) {
            uint64_t n = number(value, false);
            if (!n || n > SB_MAX_WORKERS) usage("--workers must be 1..64");
            o->scan.workers = (uint32_t)n;
        } else if (!strcmp(key, "--fd-limit")) {
            uint64_t n = number(value, false);
            if (n < 4 || n > UINT32_MAX) usage("--fd-limit must be 4..4294967295");
            o->scan.fd_limit = (uint32_t)n;
        } else if (!strcmp(key, "--timeout")) {
            uint64_t n = number(value, false);
            if (n > UINT64_MAX / UINT64_C(1000000000)) usage("timeout is too large");
            o->scan.timeout_ns = n * UINT64_C(1000000000);
        } else if (!strcmp(key, "--memory-limit")) {
            o->scan.memory_limit = number(value, true);
            if (!o->scan.memory_limit) usage("memory limit must be positive");
        } else if (!strcmp(key, "--size")) {
            if (!strcmp(value, "logical")) o->scan.size = SB_LOGICAL;
            else if (!strcmp(value, "allocated")) o->scan.size = SB_ALLOCATED;
            else usage("--size must be logical or allocated");
        } else if (!strcmp(key, "--method")) {
            o->automatic = !strcmp(value, "auto");
            if (!strcmp(value, "posix")) o->scan.method = SB_POSIX_PAR;
            else if (!strcmp(value, "bulk")) o->scan.method = SB_BULK_PAR;
            else if (o->automatic) {
#ifdef __APPLE__
                o->scan.method = SB_BULK_PAR;
#else
                o->scan.method = SB_POSIX_PAR;
#endif
            } else usage("--method must be auto, bulk, or posix");
        } else usage("unknown option");
    }
    if (!o->target) o->target = ".";
    if (o->json && o->interactive == 1) usage("--json and --interactive cannot be combined");
    const char *term = getenv("TERM");
    bool capable = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && (!term || strcmp(term, "dumb"));
    if (o->interactive == 1 && !capable) usage("--interactive requires an input/output terminal with cursor support");
    if (o->interactive < 0) o->interactive = capable;
    if (o->export && (o->json || o->interactive == 1)) usage("--export cannot be combined with --json or --interactive");
    if (o->json || o->export) o->interactive = 0;
    if (o->scan.method == SB_POSIX_PAR) o->scan.buffer_size = 0;
}

static void *show_progress(void *context) {
    Progress *p = context;
    static const char spinner[] = "|/-\\";
    unsigned frame = 0;
    pthread_mutex_lock(&p->lock);
    while (!p->done) {
        if (p->machine) {
            fprintf(stderr, "progress %" PRIu64 " %" PRIu64 "\n",
                    (uint64_t)atomic_load_explicit(&sb_progress_entries, memory_order_relaxed),
                    (sb_now_ns() - p->started) / UINT64_C(1000000));
            fflush(stderr);
        } else {
        fprintf(stderr, "\r\033[K%c Scanning ", spinner[frame++ % 4]);
        vantage_print_text(stderr, (const unsigned char *)p->config->root, strlen(p->config->root), 52);
        fprintf(stderr, "  %.1fs", (double)(sb_now_ns() - p->started) / 1e9);
        fflush(stderr);
        }
        struct timespec next;
        clock_gettime(CLOCK_REALTIME, &next);
        next.tv_nsec += 150000000;
        if (next.tv_nsec >= 1000000000) { ++next.tv_sec; next.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&p->ready, &p->lock, &next);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static void progress_start(Progress *p, const SBConfig *config, bool machine) {
    memset(p, 0, sizeof(*p));
    const char *term = getenv("TERM");
    if (!machine && (!isatty(STDERR_FILENO) || (term && !strcmp(term, "dumb")))) return;
    p->machine = machine;
    if (pthread_mutex_init(&p->lock, NULL)) return;
    if (pthread_cond_init(&p->ready, NULL)) { pthread_mutex_destroy(&p->lock); return; }
    p->config = config; p->started = sb_now_ns(); progress_visible = !machine;
    if (pthread_create(&p->thread, NULL, show_progress, p)) {
        progress_visible = 0; pthread_cond_destroy(&p->ready); pthread_mutex_destroy(&p->lock);
    } else p->running = true;
}

static void progress_finish(Progress *p) {
    if (!p->running) return;
    pthread_mutex_lock(&p->lock); p->done = true; pthread_cond_signal(&p->ready); pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    if (!p->machine) { fputs("\r\033[K", stderr); fflush(stderr); }
    progress_visible = 0;
    pthread_cond_destroy(&p->ready); pthread_mutex_destroy(&p->lock);
}

static int error_output(const Options *o, const char *reason, SBCompletion completion) {
    if (o->json) {
        fputs("{\"schema\":1,\"root\":", stdout); sb_json_string(stdout, o->target);
        fputs(",\"root_hex\":", stdout); sb_json_hex(stdout, (const unsigned char *)o->target, strlen(o->target));
        fputs(",\"completion\":", stdout); sb_json_string(stdout, sb_completion_name(completion));
        fputs(",\"reason\":", stdout); sb_json_string(stdout, reason);
        fputs(",\"graph_valid\":false,\"items\":[],\"largest_files\":[]}\n", stdout);
    }
    fputs("vantage: ", stderr);
    vantage_print_text(stderr, (const unsigned char *)reason, strlen(reason), -1); fputc('\n', stderr);
    return completion == SB_UNSUPPORTED ? 2 : 1;
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "");
    Options o; parse(&o, argc, argv);
#ifdef __APPLE__
    /* A requested scan is foreground work; new workers inherit this class. */
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    /* GUI launches normally inherit a 256-descriptor soft limit. Raise only
     * this helper's soft limit, within its existing hard limit; the scanner
     * still clamps its pool if the OS cannot grant the requested budget. */
    struct rlimit descriptors;
    if (!getrlimit(RLIMIT_NOFILE, &descriptors)) {
        rlim_t desired = (rlim_t)o.scan.fd_limit + 24;
        if (desired > descriptors.rlim_max) desired = descriptors.rlim_max;
        if (desired > descriptors.rlim_cur) {
            descriptors.rlim_cur = desired;
            (void)setrlimit(RLIMIT_NOFILE, &descriptors);
        }
    }
#endif
    char *root = realpath(o.target, NULL);
    if (!root) return error_output(&o, strerror(errno), SB_FAILED);
    if (strlen(root) >= sizeof(o.scan.root)) { free(root); return error_output(&o, "directory path is too long", SB_FAILED); }
    memcpy(o.scan.root, root, strlen(root) + 1); free(root);
    struct sigaction action = {0};
    action.sa_handler = scan_signal; sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    Progress progress; progress_start(&progress, &o.scan, o.export);
    SBResult result; SBInventory inventory;
    int status = sb_execute(&o.scan, &result, &inventory);
    progress_finish(&progress);
    if (o.automatic && o.scan.method == SB_BULK_PAR && result.completion == SB_UNSUPPORTED) {
        sb_inventory_destroy(&inventory);
        fputs("vantage: bulk scanning is unsupported here; using parallel POSIX.\n", stderr);
        o.scan.method = SB_POSIX_PAR; o.scan.buffer_size = 0;
        progress_start(&progress, &o.scan, o.export);
        status = sb_execute(&o.scan, &result, &inventory);
        progress_finish(&progress);
    }
    if (result.completion == SB_FAILED || result.completion == SB_UNSUPPORTED || !result.graph_valid) {
        int code = error_output(&o, result.reason[0] ? result.reason : "cannot construct a reliable directory view", result.completion == SB_UNSUPPORTED ? SB_UNSUPPORTED : SB_FAILED);
        sb_inventory_destroy(&inventory); return code;
    }
    VantageView view;
    if (!vantage_view_init(&view, &o.scan, &result, &inventory, !o.export)) {
        sb_inventory_destroy(&inventory);
        return error_output(&o, "invalid directory relationships in scan results", SB_FAILED);
    }
    if (o.export) vantage_export(&view, stdout);
    else if (o.json) vantage_json(&view, o.top, o.depth, o.files);
    else if (o.interactive) {
        int ui_status = vantage_tui(&view, o.files);
        if (ui_status) status = ui_status;
    } else vantage_report(&view, o.top, o.depth, o.files);
    if (fflush(stdout) == EOF || ferror(stdout)) {
        fputs("vantage: could not write output\n", stderr); status = 1;
    }
    vantage_view_destroy(&view); sb_inventory_destroy(&inventory);
    return status;
}
