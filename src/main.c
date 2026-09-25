#include "scanbench.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

typedef struct {
    SBConfig base;
    SBMethod methods[SB_METHOD_COUNT]; size_t nmethods;
    uint64_t workers[64], buffers[32], frontiers[32], shifts[16], partitions[4];
    size_t nworkers, nbuffers, nfrontiers, nshifts, npartitions;
    unsigned rounds, warmups;
    bool explicit_rounds, explicit_warmups, explicit_verify;
    bool verify;
    const char *out, *manifest, *manifest_json;
    const char *target;
} Options;
typedef struct { SBConfig config; SBResult result; const char *event; } Saved;

static void help(void) {
    puts("scanbench " SB_VERSION " - comparable macOS filesystem scan experiments\n"
         "\nUsage:\n"
         "  scanbench probe ROOT\n"
         "  scanbench scan ROOT [--method NAME] [--manifest FILE | --manifest-json FILE]\n"
         "  scanbench verify ROOT [--methods LIST]\n"
         "  scanbench bench ROOT [--methods LIST] [--out FILE]\n"
         "  scanbench report RESULTS.jsonl\n"
         "  scanbench replay INVENTORY.sbi [--rounds N] [--out FILE]\n"
         "\nMethods: posix, posix-par, bulk, bulk-par, catalog, catalog-pipe,\n"
         "         catalog-partition (experimental, opt-in). 'all' omits partitioning.\n"
         "\nCore options:\n"
         "  --task enumerate|usage|tree     Default: tree\n"
         "  --size logical|allocated       Allocated = Darwin all-forks allocation\n"
         "  --workers 1,2,4,8              Parallel configurations (max 64)\n"
         "  --buffer 16KiB,64KiB,256KiB     Bulk/catalog buffer sweep\n"
         "  --rounds N --warmup N          Default: 15 measured / 1 warmup\n"
         "  --seed N                       Seeded per-block random order\n"
         "  --verify exact|none            Warm bench default: exact preflight\n"
         "  --cache warm|first-pass|reboot-prepared|remounted-fixture\n"
         "  --consistency live|quiescent|immutable  Caller assertion, not a snapshot\n"
         "  --allow-volume-scan            Permit catalog to scan/filter a whole volume\n"
         "  --timeout SECONDS              Default: 300; 0 disables scan deadline\n"
         "  --memory-limit 1GiB            Managed-allocation cap (not total process RSS)\n"
         "  --fd-limit N --queue-limit N   Default: 256 directory FDs / 262144 tasks\n"
         "\nExperiments:\n"
         "  --order dfs|fifo|random|id-band (bounded shared frontier; default dfs)\n"
         "  --experiment id-band          Alias for --order id-band\n"
         "  --frontier 32,256,2048 --id-shift 8,12,16\n"
         "  --pack-invalid                Request fixed bulk placeholder packing\n"
         "  --skip-empty                  Requires immutable; bulk backends only\n"
         "  --adaptive                    Experimental active-worker controller\n"
         "  --reduce sort|hash            Post-scan unique-object reduction\n"
         "  --extra entrycount,linkcount,uuid   Attribute-path ablation\n"
         "  --omit-objtype                 Bulk negative control + timed enrichment\n"
         "  --partitions 1,2,4             Catalog file-ID predicate partitions\n"
         "  --catalog-retries N           Default: 2 complete restarts on EBUSY\n"
         "  --diagnostic                  Include per-batch clock instrumentation\n"
         "\nFirst-pass/prepared-cache runs require one configuration, one round, no\n"
         "warmup, and no preflight. Prepare each candidate independently.\n"
         "Replay measures the collector from an inventory, NOT filesystem I/O\n"
         "or Darwin packed-record parsing. See docs/IMPLEMENTATION.md.\n"
         "Exit codes: 0 complete, 1 failure/mismatch, 2 unsupported-only, 64 usage,\n"
         "71 allocation/system fatal error. Benchmark JSON retains failed rows.");
}
static _Noreturn void usage_error(const char *s) { fprintf(stderr, "scanbench: %s\nUse --help for usage.\n", s); exit(64); }
static uint64_t number(const char *s, bool bytes) {
    if (!s || !*s || *s == '-') usage_error("expected a nonnegative integer");
    errno = 0; char *end; unsigned long long n = strtoull(s, &end, 10);
    if (errno || end == s) usage_error("invalid or overflowing integer");
    uint64_t scale = 1;
    if (*end) {
        if (!bytes) usage_error("unexpected suffix on integer");
        if (!strcmp(end, "KiB") || !strcmp(end, "K")) scale = 1024;
        else if (!strcmp(end, "MiB") || !strcmp(end, "M")) scale = UINT64_C(1024) * 1024;
        else if (!strcmp(end, "GiB") || !strcmp(end, "G")) scale = UINT64_C(1024) * 1024 * 1024;
        else if (!strcmp(end, "B")) scale = 1;
        else usage_error("byte suffix must be B, KiB, MiB, or GiB");
    }
    if (n > UINT64_MAX / scale) usage_error("scaled integer overflow");
    return (uint64_t)n * scale;
}
static uint32_t u32(const char *s) { uint64_t n = number(s, false); if (n > UINT32_MAX) usage_error("integer exceeds 32 bits"); return (uint32_t)n; }
static size_t list_numbers(const char *str, uint64_t *out, size_t cap, bool bytes) {
    if (!str || !*str || str[0] == ',' || str[strlen(str) - 1] == ',' || strstr(str, ",,")) usage_error("empty list item");
    char *copy = strdup(str), *state = NULL; if (!copy) sb_die("out of memory");
    size_t n = 0;
    for (char *s = strtok_r(copy, ",", &state); s; s = strtok_r(NULL, ",", &state)) {
        uint64_t v = number(s, bytes); bool duplicate = false;
        for (size_t j = 0; j < n; ++j) if (out[j] == v) duplicate = true;
        if (duplicate) continue;
        if (n == cap) usage_error("too many sweep values");
        out[n++] = v;
    }
    free(copy); return n;
}
static void methods(Options *o, const char *str) {
    o->nmethods = 0;
    if (!strcmp(str, "all")) { for (int j = 0; j < SB_METHOD_COUNT - 1; ++j) o->methods[o->nmethods++] = (SBMethod)j; return; }
    char *copy = strdup(str), *state = NULL; if (!copy) sb_die("out of memory");
    for (char *s = strtok_r(copy, ",", &state); s; s = strtok_r(NULL, ",", &state)) {
        bool found = false;
        for (int j = 0; j < SB_METHOD_COUNT; ++j) if (!strcmp(s, sb_method_name((SBMethod)j))) {
            found = true; bool dupe = false;
            for (size_t k = 0; k < o->nmethods; ++k) if (o->methods[k] == (SBMethod)j) dupe = true;
            if (!dupe) o->methods[o->nmethods++] = (SBMethod)j;
        }
        if (!found) usage_error("unknown method; raw APFS and persistent refresh are not implemented backends");
    }
    free(copy); if (!o->nmethods) usage_error("empty method list");
}
static void extras(SBConfig *c, const char *str) {
    char *copy = strdup(str), *state = NULL; if (!copy) sb_die("out of memory");
    for (char *s = strtok_r(copy, ",", &state); s; s = strtok_r(NULL, ",", &state)) {
        if (!strcmp(s, "entrycount")) c->extra_entrycount = true;
        else if (!strcmp(s, "linkcount")) c->extra_linkcount = true;
        else if (!strcmp(s, "uuid")) c->extra_uuid = true;
        else usage_error("--extra accepts entrycount,linkcount,uuid");
    }
    free(copy);
}
static void parse(Options *o, int argc, char **argv) {
    memset(o, 0, sizeof(*o));
    o->base.task = SB_TREE; o->base.size = SB_LOGICAL; o->base.workers = 4;
    if (!strcmp(argv[1], "scan")) o->base.cache = SB_CACHE_FIRST;
    o->base.frontier = 256; o->base.id_shift = 12; o->base.queue_limit = 262144; o->base.fd_limit = 256;
    o->base.buffer_size = 256 * 1024; o->base.memory_limit = UINT64_C(1024) * 1024 * 1024;
    o->base.timeout_ns = UINT64_C(300000000000); o->base.catalog_retries = 2;
    o->base.catalog_partitions = 2; o->base.seed = 42; o->base.session = sb_mix(sb_realtime_ns() ^ (uint64_t)getpid());
    o->workers[0] = 4; o->nworkers = 1; o->buffers[0] = o->base.buffer_size; o->nbuffers = 1;
    o->frontiers[0] = 256; o->nfrontiers = 1; o->shifts[0] = 12; o->nshifts = 1;
    o->partitions[0] = 2; o->npartitions = 1;
    o->rounds = 15; o->warmups = 1; o->verify = true;
    methods(o, !strcmp(argv[1], "scan") ? "posix" : "posix,bulk,bulk-par,catalog,catalog-pipe");
    for (int j = 2; j < argc; ++j) {
        const char *arg = argv[j];
        if (!strcmp(arg, "--")) { if (++j != argc - 1 || o->target) usage_error("expected one target after --"); o->target = argv[j]; break; }
        if (strncmp(arg, "--", 2)) { if (o->target) usage_error("multiple targets supplied"); o->target = arg; continue; }
        char key[80]; const char *equal = strchr(arg, '='); size_t len = equal ? (size_t)(equal - arg) : strlen(arg);
        if (len >= sizeof(key)) usage_error("option name too long");
        memcpy(key, arg, len); key[len] = 0;
#define FLAG(name, field) if (!strcmp(key, name)) { if (equal) usage_error("boolean flag takes no value"); o->base.field = true; continue; }
        FLAG("--pack-invalid", pack_invalid); FLAG("--skip-empty", skip_empty); FLAG("--adaptive", adaptive);
        FLAG("--allow-volume-scan", allow_volume_scan); FLAG("--omit-objtype", omit_objtype); FLAG("--diagnostic", diagnostic);
#undef FLAG
        const char *v = equal ? equal + 1 : ++j < argc ? argv[j] : NULL;
        if (!v || !*v || !strncmp(v, "--", 2)) usage_error("missing option value");
        if (!strcmp(key, "--methods") || !strcmp(key, "--method")) methods(o, v);
        else if (!strcmp(key, "--workers")) o->nworkers = list_numbers(v, o->workers, 64, false);
        else if (!strcmp(key, "--buffer")) o->nbuffers = list_numbers(v, o->buffers, 32, true);
        else if (!strcmp(key, "--frontier")) o->nfrontiers = list_numbers(v, o->frontiers, 32, false);
        else if (!strcmp(key, "--id-shift")) o->nshifts = list_numbers(v, o->shifts, 16, false);
        else if (!strcmp(key, "--partitions")) o->npartitions = list_numbers(v, o->partitions, 4, false);
        else if (!strcmp(key, "--rounds")) { o->rounds = u32(v); o->explicit_rounds = true; }
        else if (!strcmp(key, "--warmup")) { o->warmups = u32(v); o->explicit_warmups = true; }
        else if (!strcmp(key, "--seed")) o->base.seed = number(v, false);
        else if (!strcmp(key, "--fd-limit")) o->base.fd_limit = u32(v);
        else if (!strcmp(key, "--queue-limit")) o->base.queue_limit = u32(v);
        else if (!strcmp(key, "--memory-limit")) o->base.memory_limit = number(v, true);
        else if (!strcmp(key, "--catalog-retries")) o->base.catalog_retries = u32(v);
        else if (!strcmp(key, "--timeout")) { uint64_t sec = number(v, false); if (sec > UINT64_MAX / 1000000000) usage_error("timeout overflow"); o->base.timeout_ns = sec * 1000000000; }
        else if (!strcmp(key, "--out")) o->out = v;
        else if (!strcmp(key, "--manifest")) o->manifest = v;
        else if (!strcmp(key, "--manifest-json")) o->manifest_json = v;
        else if (!strcmp(key, "--task")) {
            if (!strcmp(v, "tree")) o->base.task = SB_TREE;
            else if (!strcmp(v, "usage")) o->base.task = SB_USAGE;
            else if (!strcmp(v, "enumerate")) o->base.task = SB_ENUMERATE;
            else usage_error("task must be tree, usage, or enumerate; refresh is not implemented");
        } else if (!strcmp(key, "--size")) {
            if (!strcmp(v, "logical")) o->base.size = SB_LOGICAL;
            else if (!strcmp(v, "allocated")) o->base.size = SB_ALLOCATED;
            else usage_error("size must be logical or allocated");
        } else if (!strcmp(key, "--consistency")) {
            if (!strcmp(v, "live") || !strcmp(v, "live-best-effort")) o->base.consistency = SB_LIVE;
            else if (!strcmp(v, "quiescent")) o->base.consistency = SB_QUIESCENT;
            else if (!strcmp(v, "immutable")) o->base.consistency = SB_IMMUTABLE;
            else usage_error("consistency must be live, quiescent, or immutable");
        } else if (!strcmp(key, "--cache")) {
            if (!strcmp(v, "warm")) o->base.cache = SB_CACHE_WARM;
            else if (!strcmp(v, "first-pass")) o->base.cache = SB_CACHE_FIRST;
            else if (!strcmp(v, "reboot-prepared")) o->base.cache = SB_CACHE_REBOOT;
            else if (!strcmp(v, "remounted-fixture")) o->base.cache = SB_CACHE_REMOUNT;
            else usage_error("unknown cache regime");
        } else if (!strcmp(key, "--verify")) {
            o->explicit_verify = true;
            if (!strcmp(v, "exact")) o->verify = true;
            else if (!strcmp(v, "none")) o->verify = false;
            else usage_error("--verify is exact or none");
        } else if (!strcmp(key, "--order") || !strcmp(key, "--experiment")) {
            if (!strcmp(v, "dfs")) o->base.order = SB_DFS;
            else if (!strcmp(v, "fifo")) o->base.order = SB_FIFO;
            else if (!strcmp(v, "random")) o->base.order = SB_RANDOM;
            else if (!strcmp(v, "id-band")) o->base.order = SB_ID_BAND;
            else usage_error("unknown scheduling order");
        } else if (!strcmp(key, "--reduce")) {
            if (!strcmp(v, "sort")) o->base.reduce = SB_REDUCE_SORT;
            else if (!strcmp(v, "hash")) o->base.reduce = SB_REDUCE_HASH;
            else usage_error("--reduce is sort or hash");
        } else if (!strcmp(key, "--extra")) extras(&o->base, v);
        else usage_error("unknown option");
    }
    if (!o->target) usage_error("missing target");
    if (o->rounds < 1 || o->rounds > 10000 || o->warmups > 1000) usage_error("rounds must be 1..10000 and warmup 0..1000");
    if (!o->base.queue_limit || o->base.fd_limit < 4 || !o->base.memory_limit) usage_error("resource limits must be positive (fd-limit at least 4)");
    if (o->base.catalog_retries > 100) usage_error("catalog retries must be at most 100");
    for (size_t j = 0; j < o->nworkers; ++j) if (!o->workers[j] || o->workers[j] > SB_MAX_WORKERS) usage_error("workers must be 1..64");
    for (size_t j = 0; j < o->nbuffers; ++j) if (o->buffers[j] < 1024 || o->buffers[j] > 64 * 1024 * 1024) usage_error("buffers must be 1KiB..64MiB");
    for (size_t j = 0; j < o->nfrontiers; ++j) if (!o->frontiers[j] || o->frontiers[j] > UINT32_MAX) usage_error("invalid frontier");
    for (size_t j = 0; j < o->nshifts; ++j) if (o->shifts[j] > 63) usage_error("id-shift must be 0..63");
    for (size_t j = 0; j < o->npartitions; ++j) if (o->partitions[j] != 1 && o->partitions[j] != 2 && o->partitions[j] != 4) usage_error("catalog partitions must be 1, 2, or 4");
    if (o->base.skip_empty && o->base.consistency != SB_IMMUTABLE) usage_error("--skip-empty requires --consistency immutable");
    if (o->manifest && o->manifest_json) usage_error("choose one manifest format");
    if ((o->manifest || o->manifest_json) && strcmp(argv[1], "scan")) usage_error("manifest export is available on scan only");
    const char *mp = o->manifest ? o->manifest : o->manifest_json;
    if (mp && o->out && !strcmp(mp, o->out)) usage_error("manifest and result paths must differ");
    if (mp && !strcmp(mp, "-") && (!o->out || !strcmp(o->out, "-")))
        usage_error("a stdout manifest requires --out FILE for the separate result");
    const char *outputs[] = {o->out, mp};
    for (size_t k = 0; k < 2; ++k) if (outputs[k] && strcmp(outputs[k], "-")) {
        struct stat exists;
        if (!lstat(outputs[k], &exists)) usage_error("output already exists; choose a new path (nothing is overwritten)");
        if (errno != ENOENT) usage_error("output path cannot be checked");
    }
    if (o->base.cache != SB_CACHE_WARM) {
        if (!o->explicit_rounds) o->rounds = 1;
        if (!o->explicit_warmups) o->warmups = 0;
        if (!o->explicit_verify) o->verify = false;
    }
}
static size_t configurations(const Options *o, SBConfig *configs) {
    size_t n = 0;
    for (size_t m = 0; m < o->nmethods; ++m) {
        SBMethod method = o->methods[m]; bool parallel = method == SB_POSIX_PAR || method == SB_BULK_PAR;
        bool walk = method < SB_CATALOG, bulk = method == SB_BULK || method == SB_BULK_PAR;
        size_t wn = parallel ? o->nworkers : 1, bn = bulk || !walk ? o->nbuffers : 1;
        size_t fn = walk && o->base.order == SB_ID_BAND ? o->nfrontiers : 1;
        size_t sn = walk && o->base.order == SB_ID_BAND ? o->nshifts : 1;
        size_t pn = method == SB_CATALOG_PARTITION ? o->npartitions : 1;
        for (size_t w = 0; w < wn; ++w) for (size_t b = 0; b < bn; ++b)
        for (size_t f = 0; f < fn; ++f) for (size_t s = 0; s < sn; ++s) for (size_t p = 0; p < pn; ++p) {
            if (n == SB_MAX_CONFIGS) usage_error("configuration sweep exceeds 4096 configurations");
            SBConfig c = o->base; c.method = method; c.config_index = (uint32_t)n;
            c.workers = parallel ? (uint32_t)o->workers[w] : method == SB_CATALOG_PIPE ? 2 : 1;
            c.buffer_size = bulk || !walk ? o->buffers[b] : 0;
            c.frontier = walk && c.order == SB_ID_BAND ? (uint32_t)o->frontiers[f] : 0;
            c.id_shift = walk && c.order == SB_ID_BAND ? (uint32_t)o->shifts[s] : 0;
            c.catalog_partitions = method == SB_CATALOG_PARTITION ? (uint32_t)o->partitions[p] : 1;
            if (method == SB_CATALOG_PARTITION) c.workers = c.catalog_partitions;
            c.adaptive &= parallel;
            if (!bulk) { c.pack_invalid = false; c.skip_empty = false; c.omit_objtype = false; }
            if (!bulk && walk) { c.extra_entrycount = false; c.extra_linkcount = false; c.extra_uuid = false; }
            if (!walk) c.order = SB_DFS;
            configs[n++] = c;
        }
    }
    return n;
}
static void find_self(char *out, size_t size, const char *argv0) {
#ifdef __APPLE__
    uint32_t n = (uint32_t)size;
    if (_NSGetExecutablePath(out, &n)) sb_die("executable path too long");
#else
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n < 0) { if (!realpath(argv0, out)) sb_die("cannot resolve executable path"); }
    else out[n] = 0;
#endif
    (void)argv0;
}
static bool digest_equal(const SBResult *a, const SBResult *b) {
    return a->root_device == b->root_device && a->root_id == b->root_id && a->entries == b->entries &&
        a->digest_a == b->digest_a && a->digest_b == b->digest_b && a->digest_c == b->digest_c &&
        a->bytes == b->bytes && a->unique_bytes == b->unique_bytes;
}
static void verify_one(const char *self, const SBConfig *cfg, const SBResult *gold,
                        SBInventory *gold_inventory, SBResult *result) {
    SBConfig c = *cfg; c.keep_manifest = true;
    SBInventory inventory; sb_spawn(self, &c, result, &inventory);
    if (result->completion == SB_COMPLETE && gold->completion == SB_COMPLETE) {
        char reason[256]; bool same = sb_inventory_equal(gold_inventory, &inventory, reason, sizeof(reason));
        result->verification = same ? SB_VERIFIED : SB_MISMATCH;
        snprintf(result->verification_basis, sizeof(result->verification_basis), "exact-posix-manifest");
        if (!same) snprintf(result->reason, sizeof(result->reason), "%s", reason);
    } else if (gold->completion != SB_COMPLETE && result->completion == SB_COMPLETE)
        snprintf(result->reason, sizeof(result->reason), "reference scan is incomplete; cannot certify equality");
    sb_inventory_destroy(&inventory);
}
static FILE *open_output(const char *path) {
    if (!path || !strcmp(path, "-")) return stdout;
    FILE *f = fopen(path, "wx");
    if (!f) sb_die("cannot create output '%s': %s (existing files are not overwritten)", path, strerror(errno));
    return f;
}
static void close_output(FILE *f) {
    int error = fflush(f);
    if (fclose(f)) error = 1;
    if (error) sb_die("writing output failed");
}

static int run_comparison(const char *self, Options *o, SBConfig *configs, size_t n, bool verify_only) {
    if (!verify_only && o->base.cache != SB_CACHE_WARM && (n != 1 || o->rounds != 1 || o->warmups || o->verify))
        usage_error("a prepared/first-pass run must have one configuration, one round, no warmup, and --verify none");
    if (!verify_only && o->base.cache == SB_CACHE_WARM && !o->verify && !o->warmups)
        usage_error("a warm benchmark needs exact preflight or at least one warmup; use first-pass for an unprepared trial");
    size_t events = verify_only ? n + 1 : (size_t)(o->rounds + o->warmups + 1) * n + 1;
    if (events > 100000) usage_error("too many total trials (limit 100000)");
    Saved *saved = calloc(events, sizeof(*saved)); SBResult *checked = calloc(n, sizeof(*checked));
    bool *eligible = calloc(n, sizeof(*eligible)); size_t *order = calloc(n, sizeof(*order));
    if (!saved || !checked || !eligible || !order) sb_die("benchmark bookkeeping allocation failed");
    size_t nsaved = 0; SBResult golden = {0}; SBInventory gold_inventory = {0};
    bool preflight = verify_only || o->verify, reference_failed = false;
    for (size_t j = 0; j < n; ++j) eligible[j] = true;
    if (preflight) {
        fprintf(stderr, "Exact manifest preflight (%zu configurations); this warms target metadata.\n", n);
        SBConfig reference = o->base; reference.method = SB_POSIX; reference.workers = 1;
        reference.buffer_size = 0; reference.pack_invalid = reference.skip_empty = reference.adaptive = reference.omit_objtype = false;
        reference.extra_uuid = reference.extra_entrycount = reference.extra_linkcount = false;
        reference.order = SB_DFS; reference.frontier = reference.id_shift = 0;
        reference.catalog_partitions = 1; reference.keep_manifest = true;
        sb_spawn(self, &reference, &golden, &gold_inventory);
        saved[nsaved++] = (Saved){reference, golden, "reference"};
        reference_failed = golden.completion != SB_COMPLETE;
        if (reference_failed)
            fprintf(stderr, "Exact reference is %s: %s; no configurations verified or measured.\n",
                sb_completion_name(golden.completion), golden.reason);
        for (size_t j = 0; !reference_failed && j < n; ++j) {
            verify_one(self, configs + j, &golden, &gold_inventory, checked + j);
            SBConfig c = configs[j]; c.keep_manifest = true;
            saved[nsaved++] = (Saved){c, checked[j], "verification"};
            fprintf(stderr, "  %s (%zu/%zu): %s / %s\n", sb_method_name(c.method), j + 1, n,
                sb_completion_name(checked[j].completion), sb_verification_name(checked[j].verification));
            if (checked[j].completion == SB_UNSUPPORTED || checked[j].completion == SB_FAILED) eligible[j] = false;
        }
        sb_inventory_destroy(&gold_inventory);
    }
    if (!verify_only && !reference_failed) {
        uint64_t rng = o->base.seed;
        unsigned blocks = o->warmups + o->rounds;
        for (unsigned block = 0; block < blocks; ++block) {
            for (size_t j = 0; j < n; ++j) order[j] = j;
            for (size_t j = n; j > 1; --j) { size_t k = (size_t)(sb_random(&rng) % j), t = order[j - 1]; order[j - 1] = order[k]; order[k] = t; }
            bool warmup = block < o->warmups;
            fprintf(stderr, "%s block %u/%u\n", warmup ? "Warmup" : "Measured", warmup ? block + 1 : block - o->warmups + 1, warmup ? o->warmups : o->rounds);
            for (size_t k = 0; k < n; ++k) {
                size_t j = order[k]; if (!eligible[j]) continue;
                SBConfig c = configs[j]; c.keep_manifest = false; c.round = warmup ? block : block - o->warmups;
                SBResult result; SBInventory inventory; sb_spawn(self, &c, &result, &inventory); sb_inventory_destroy(&inventory);
                if (preflight && checked[j].verification == SB_MISMATCH) {
                    result.verification = SB_MISMATCH;
                    snprintf(result.verification_basis, sizeof(result.verification_basis), "exact-preflight-mismatch");
                    if (result.reason[0]) {
                        char trial_reason[sizeof(result.reason)];
                        memcpy(trial_reason, result.reason, sizeof(trial_reason));
                        snprintf(result.reason, sizeof(result.reason), "preflight mismatch: %.108s; trial: %.118s",
                            checked[j].reason, trial_reason);
                    } else snprintf(result.reason, sizeof(result.reason), "preflight mismatch: %.235s", checked[j].reason);
                } else if (preflight && result.completion == SB_COMPLETE && checked[j].verification == SB_VERIFIED) {
                    bool same = digest_equal(&golden, &result);
                    result.verification = same ? SB_VERIFIED : SB_MISMATCH;
                    snprintf(result.verification_basis, sizeof(result.verification_basis), "exact-preflight+trial-digest");
                    if (!same) snprintf(result.reason, sizeof(result.reason), "trial inventory differs from preflight; possible live changes");
                }
                saved[nsaved++] = (Saved){c, result, warmup ? "warmup" : "trial"};
                /* A rejected API set need not be retried fifteen times. */
                if (result.completion == SB_UNSUPPORTED) eligible[j] = false;
            }
        }
    }
    /* All target scans have ended before the output path is created. */
    FILE *out = open_output(o->out); int code = 2; bool failure = reference_failed;
    for (size_t j = 0; j < nsaved; ++j) {
        sb_result_json(out, &saved[j].config, &saved[j].result, saved[j].event);
        if (!strcmp(saved[j].event, "reference") || !strcmp(saved[j].event, "warmup")) continue;
        if (saved[j].result.completion == SB_COMPLETE) code = 0;
        if (saved[j].result.completion == SB_FAILED || saved[j].result.completion == SB_PARTIAL || saved[j].result.verification == SB_MISMATCH) failure = true;
    }
    close_output(out); free(saved); free(checked); free(eligible); free(order);
    return failure ? 1 : code;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { help(); return 0; }
    if (!strcmp(argv[1], "--version")) { puts(SB_VERSION); return 0; }
    if (!strcmp(argv[1], "__run")) {
        if (argc != 4) return 64;
        int in = (int)u32(argv[2]), out = (int)u32(argv[3]); SBConfig cfg;
        if (!sb_read_all(in, &cfg, sizeof(cfg))) return 71;
        close(in);
        cfg.root[sizeof(cfg.root) - 1] = 0;
        SBResult result; SBInventory inventory; int code = sb_execute(&cfg, &result, &inventory);
        if (!sb_write_all(out, &result, sizeof(result))) return 71;
        if (cfg.keep_manifest && !sb_manifest_write(out, &inventory)) return 71;
        sb_inventory_destroy(&inventory); close(out); return code;
    }
    if (!strcmp(argv[1], "report")) { if (argc != 3) usage_error("report needs exactly one JSON Lines file"); return sb_report(argv[2]); }
    if (strcmp(argv[1], "scan") && strcmp(argv[1], "bench") && strcmp(argv[1], "verify") && strcmp(argv[1], "probe") && strcmp(argv[1], "replay")) usage_error("unknown command");
    for (int j = 2; j < argc; ++j) if (!strcmp(argv[j], "--help")) { help(); return 0; }
    Options opts; parse(&opts, argc, argv);
    if (!strcmp(argv[1], "replay")) {
        /* Output creation is deferred by replay until all measurements finish. */
        FILE *tmp = tmpfile(); if (!tmp) sb_die("temporary replay output failed");
        int code = sb_replay(opts.target, &opts.base, opts.rounds, tmp);
        if (fseek(tmp, 0, SEEK_SET)) sb_die("rewinding replay output failed");
        FILE *out = open_output(opts.out); char b[8192]; size_t n;
        while ((n = fread(b, 1, sizeof(b), tmp))) if (fwrite(b, 1, n, out) != n) sb_die("replay output write failed");
        if (ferror(tmp)) sb_die("reading temporary replay output failed");
        fclose(tmp); close_output(out); return code;
    }
    char *root = realpath(opts.target, NULL); if (!root) { fprintf(stderr, "scanbench: root: %s\n", strerror(errno)); return 1; }
    if (strlen(root) >= sizeof(opts.base.root)) usage_error("root path exceeds internal limit");
    memcpy(opts.base.root, root, strlen(root) + 1); free(root);
    if (!strcmp(argv[1], "probe")) {
        FILE *out = open_output(opts.out); int code = sb_probe(&opts.base, out);
        close_output(out); return code;
    }
    SBConfig *configs = calloc(SB_MAX_CONFIGS, sizeof(*configs)); if (!configs) sb_die("configuration allocation failed");
    size_t n = configurations(&opts, configs); char self[SB_PATH_CAP]; find_self(self, sizeof(self), argv[0]);
    if (!strcmp(argv[1], "scan")) {
        if (n != 1) usage_error("scan accepts exactly one configuration; use bench for sweeps");
        configs[0].keep_manifest = opts.manifest || opts.manifest_json;
        SBResult result; SBInventory inventory;
        int code = sb_spawn(self, configs, &result, &inventory);
        if (opts.manifest || opts.manifest_json) {
            FILE *manifest = open_output(opts.manifest ? opts.manifest : opts.manifest_json);
            sb_inventory_sort(&inventory);
            if (opts.manifest) { if (!sb_manifest_write(fileno(manifest), &inventory)) sb_die("writing manifest failed"); }
            else sb_manifest_json(manifest, &inventory);
            close_output(manifest);
        }
        sb_inventory_destroy(&inventory);
        FILE *out = open_output(opts.out); sb_result_json(out, configs, &result, "scan"); close_output(out); free(configs); return code;
    }
    int code = run_comparison(self, &opts, configs, n, !strcmp(argv[1], "verify")); free(configs); return code;
}
