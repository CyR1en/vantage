/* Replace child-process execution only in this test translation unit. The
 * production comparison, manifest comparison, JSON and report paths stay real. */
#define sb_spawn comparison_spawn
#define main comparison_cli_main
#include "../src/main.c"
#undef main
#undef sb_spawn

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static SBCompletion reference_completion;
static bool mismatch_preflight, fail_mismatching_trial;
static unsigned spawn_calls;

int comparison_spawn(const char *self, const SBConfig *cfg, SBResult *r, SBInventory *inventory) {
    (void)self; ++spawn_calls;
    memset(r, 0, sizeof(*r)); memset(inventory, 0, sizeof(*inventory));
    r->completion = SB_COMPLETE; r->verification = SB_UNVERIFIED;
    snprintf(r->verification_basis, sizeof(r->verification_basis), "none");
    if (spawn_calls == 1 && reference_completion != SB_COMPLETE) {
        sb_failure(r, reference_completion, EIO, "injected reference failure");
        return reference_completion == SB_UNSUPPORTED ? 2 : 1;
    }
    r->root_device = 7; r->root_id = 10; r->group_id = 123;
    SBCollector collector; sb_collector_init(&collector, cfg->keep_manifest || cfg->task == SB_TREE);
    SBEntry entry = {.device=7, .object_id=11, .parent_id=10, .kind=1,
        .valid=SB_REQUIRED | SB_VALID_SIZE, .size=16, .name_length=1};
    /* Measured candidates match the reference again. A matching digest cannot
     * erase the exact manifest mismatch that happened during preflight. */
    if (mismatch_preflight && cfg->method == SB_BULK && cfg->keep_manifest) ++entry.size;
    sb_collect(&collector, cfg, entry, "f");
    sb_collect_finalize(&collector, 1, cfg, r, inventory);
    r->total_ns = cfg->method == SB_POSIX ? 1000000 : 500000;
    r->scan_ns = r->total_ns; r->process_wall_ns = r->total_ns + 100000;
    if (fail_mismatching_trial && cfg->method == SB_BULK && !cfg->keep_manifest && cfg->round == 1)
        sb_failure(r, SB_FAILED, EIO, "injected trial failure");
    return r->completion == SB_COMPLETE ? 0 : 1;
}

static char *read_stream(FILE *f) {
    CHECK(!fseek(f, 0, SEEK_END)); long length = ftell(f); CHECK(length >= 0);
    CHECK(!fseek(f, 0, SEEK_SET));
    char *text = malloc((size_t)length + 1); CHECK(text);
    CHECK(fread(text, 1, (size_t)length, f) == (size_t)length);
    text[length] = 0; return text;
}
static char *read_path(const char *path) {
    FILE *f = fopen(path, "r"); CHECK(f);
    char *text = read_stream(f); CHECK(!fclose(f)); return text;
}
static char *capture_report(const char *path, int expected_code) {
    FILE *capture = tmpfile(); CHECK(capture);
    CHECK(!fflush(stdout)); int saved = dup(STDOUT_FILENO); CHECK(saved >= 0);
    CHECK(dup2(fileno(capture), STDOUT_FILENO) >= 0);
    int code = sb_report(path);
    CHECK(!fflush(stdout)); CHECK(dup2(saved, STDOUT_FILENO) >= 0); CHECK(!close(saved));
    char *text = read_stream(capture); CHECK(!fclose(capture));
    CHECK(code == expected_code); return text;
}
static void reset_case(void) {
    reference_completion = SB_COMPLETE; mismatch_preflight = fail_mismatching_trial = false; spawn_calls = 0;
}
static Options options(const char *output) {
    Options o = {.out=output, .verify=true, .rounds=3, .warmups=1};
    o.base.task = SB_TREE; o.base.size = SB_LOGICAL; o.base.cache = SB_CACHE_WARM;
    o.base.workers = 1; o.base.seed = 42; o.base.session = 17;
    snprintf(o.base.root, sizeof(o.base.root), "/mock");
    return o;
}
static int compare(Options *o, bool verify_only) {
    SBConfig configs[2] = {o->base, o->base};
    configs[0].method = SB_POSIX; configs[1].method = SB_BULK; configs[1].config_index = 1;
    return run_comparison("/unused-test-executable", o, configs, 2, verify_only);
}
static void check_trials(const char *path, bool mismatch, bool trial_failure, bool verified) {
    char *text = read_path(path), *state = NULL;
    unsigned trial_count = 0, bad_trials = 0, bad_warmups = 0, failed_trials = 0;
    for (char *line = strtok_r(text, "\n", &state); line; line = strtok_r(NULL, "\n", &state)) {
        bool trial = strstr(line, "\"event\":\"trial\"") != NULL;
        bool warmup = strstr(line, "\"event\":\"warmup\"") != NULL;
        if (!trial && !warmup) continue;
        if (trial) ++trial_count;
        if (mismatch && strstr(line, "\"method\":\"bulk\"")) {
            CHECK(strstr(line, "\"verification\":\"mismatch\""));
            CHECK(strstr(line, "\"verification_basis\":\"exact-preflight-mismatch\""));
            CHECK(strstr(line, "preflight mismatch: normalized entry 0 differs"));
            if (trial) ++bad_trials; else ++bad_warmups;
            if (trial_failure && trial && strstr(line, "\"round\":1,")) {
                CHECK(strstr(line, "\"completion\":\"failed\""));
                CHECK(strstr(line, "injected trial failure")); ++failed_trials;
            } else CHECK(strstr(line, "\"completion\":\"complete\""));
        } else CHECK(strstr(line, verified ? "\"verification\":\"verified\"" : "\"verification\":\"unverified\""));
    }
    CHECK(trial_count == 6); CHECK(bad_trials == (mismatch ? 3u : 0u));
    CHECK(bad_warmups == (mismatch ? 1u : 0u)); CHECK(failed_trials == (trial_failure ? 1u : 0u));
    free(text);
}
static void run_complete_cases(const char *path) {
    reset_case(); Options o = options(path);
    CHECK(compare(&o, false) == 0); CHECK(spawn_calls == 11);
    check_trials(path, false, false, true);
    char *report = capture_report(path, 0);
    CHECK(strstr(report, "Scan trials: 6 complete (6 verified, 0 known mismatch, 0 unverified); 0 failed, 0 partial, 0 unsupported."));
    CHECK(strstr(report, "Paired speedup vs POSIX: 2.000x; 95% percentile bootstrap [2.000x, 2.000x], 3 pairs."));
    free(report); CHECK(!unlink(path));

    reset_case(); o.verify = false;
    CHECK(compare(&o, false) == 0); CHECK(spawn_calls == 8);
    check_trials(path, false, false, false);
    report = capture_report(path, 0);
    CHECK(strstr(report, "Scan trials: 6 complete (0 verified, 0 known mismatch, 6 unverified)"));
    CHECK(strstr(report, "UNVERIFIED / exploratory")); CHECK(!strstr(report, "Paired speedup"));
    free(report); CHECK(!unlink(path));
}
static void run_mismatch_cases(const char *path) {
    for (unsigned failure = 0; failure < 2; ++failure) {
        reset_case(); mismatch_preflight = true; fail_mismatching_trial = failure != 0;
        Options o = options(path); CHECK(compare(&o, false) == 1); CHECK(spawn_calls == 11);
        check_trials(path, true, failure != 0, true);
        char *report = capture_report(path, 0);
        CHECK(strstr(report, failure ?
            "Scan trials: 5 complete (3 verified, 2 known mismatch, 0 unverified); 1 failed, 0 partial, 0 unsupported." :
            "Scan trials: 6 complete (3 verified, 3 known mismatch, 0 unverified); 0 failed, 0 partial, 0 unsupported."));
        CHECK(strstr(report, "Preflight records: 2 complete (1 verified, 1 known mismatch, 0 unverified)"));
        CHECK(!strstr(report, "\nbulk |")); CHECK(!strstr(report, "Paired speedup"));
        CHECK(!strstr(report, "non-mismatching")); free(report); CHECK(!unlink(path));
    }
}
static void run_reference_failures(const char *path) {
    const SBCompletion failures[] = {SB_FAILED, SB_PARTIAL, SB_UNSUPPORTED};
    const char *summaries[] = {"1 failed, 0 partial, 0 unsupported.", "0 failed, 1 partial, 0 unsupported.", "0 failed, 0 partial, 1 unsupported."};
    for (size_t j = 0; j < sizeof(failures) / sizeof(failures[0]); ++j) {
        for (unsigned verify_only = 0; verify_only < 2; ++verify_only) {
            reset_case(); reference_completion = failures[j]; Options o = options(path);
            CHECK(compare(&o, verify_only != 0) == 1); CHECK(spawn_calls == 1);
            char *text = read_path(path);
            CHECK(strstr(text, "\"event\":\"reference\"")); CHECK(strstr(text, "injected reference failure"));
            CHECK(!strstr(text, "\"event\":\"trial\"")); CHECK(!strstr(text, "\"event\":\"verification\""));
            free(text);
            /* The would-be candidate succeeds; only the reference was faulty. */
            SBResult r; SBInventory inventory;
            CHECK(comparison_spawn("unused", &o.base, &r, &inventory) == 0); sb_inventory_destroy(&inventory);
            char *report = capture_report(path, 0);
            char expected[256]; snprintf(expected, sizeof(expected),
                "Reference records: 0 complete (0 verified, 0 known mismatch, 0 unverified); %s", summaries[j]);
            CHECK(strstr(report, expected)); CHECK(strstr(report, "No eligible scan timings."));
            CHECK(!strstr(report, "Paired speedup")); free(report); CHECK(!unlink(path));
        }
    }
}
static void run_report_statuses(const char *path) {
    Options o = options(path); SBConfig cfg = o.base;
    const SBCompletion completions[] = {SB_COMPLETE, SB_COMPLETE, SB_COMPLETE, SB_FAILED, SB_PARTIAL, SB_UNSUPPORTED};
    const SBVerification verifications[] = {SB_VERIFIED, SB_MISMATCH, SB_UNVERIFIED, SB_UNVERIFIED, SB_UNVERIFIED, SB_UNVERIFIED};
    FILE *f = fopen(path, "w"); CHECK(f);
    for (size_t j = 0; j < sizeof(completions) / sizeof(completions[0]); ++j) {
        SBResult r = {.completion=completions[j], .verification=verifications[j], .total_ns=1000};
        sb_result_json(f, &cfg, &r, "trial");
    }
    CHECK(!fclose(f)); char *report = capture_report(path, 0);
    CHECK(strstr(report, "Scan trials: 3 complete (1 verified, 1 known mismatch, 1 unverified); 1 failed, 1 partial, 1 unsupported."));
    free(report); CHECK(!unlink(path));

    f = fopen(path, "w"); CHECK(f);
    fputs("{\"schema\":1,\"event\":\"trial\",\"completion\":\"complete\",\"verification\":\"misspelled-mismatch\"}\n", f);
    CHECK(!fclose(f)); report = capture_report(path, 1); free(report); CHECK(!unlink(path));
}
int main(void) {
    char work[] = "/tmp/scanbench-comparison-XXXXXX"; CHECK(mkdtemp(work));
    char path[SB_PATH_CAP]; snprintf(path, sizeof(path), "%s/results.jsonl", work);
    run_reference_failures(path); run_mismatch_cases(path); run_complete_cases(path); run_report_statuses(path);
    CHECK(!rmdir(work));
    puts("comparison mock: exact-reference failures, persistent preflight mismatches, trial failures, report status counts and paired arithmetic passed");
    return 0;
}
