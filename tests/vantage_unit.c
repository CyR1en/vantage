#include "vantage.h"
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static SBConfig config(void) {
    SBConfig cfg = {.task = SB_TREE, .size = SB_LOGICAL, .method = SB_POSIX,
        .workers = 1, .keep_manifest = true};
    strcpy(cfg.root, "/tmp/vantage-unit");
    return cfg;
}

static char *read_stream(FILE *stream) {
    CHECK(!fflush(stream));
    CHECK(!fseek(stream, 0, SEEK_END));
    long end = ftell(stream);
    CHECK(end >= 0);
    rewind(stream);
    size_t size = (size_t)end;
    char *text = sb_alloc(size + 1);
    CHECK(fread(text, 1, size, stream) == size);
    text[size] = 0;
    return text;
}

static char *capture(const VantageView *view, bool json, size_t top, unsigned depth, bool files) {
    CHECK(!fflush(stdout));
    FILE *stream = tmpfile(); CHECK(stream);
    int saved = dup(STDOUT_FILENO); CHECK(saved >= 0);
    CHECK(dup2(fileno(stream), STDOUT_FILENO) == STDOUT_FILENO);
    if (json) vantage_json(view, top, depth, files);
    else vantage_report(view, top, depth, files);
    CHECK(!fflush(stdout));
    CHECK(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    CHECK(!close(saved));
    char *text = read_stream(stream);
    CHECK(!fclose(stream));
    return text;
}

static void add(SBCollector *collector, const SBConfig *cfg, uint64_t id,
                uint64_t parent, uint32_t kind, uint64_t bytes, bool known, const char *name) {
    SBEntry entry = {.device = 42, .object_id = id, .parent_id = parent,
        .kind = kind, .size = bytes, .valid = SB_REQUIRED | (known ? SB_VALID_SIZE : 0),
        .name_length = (uint32_t)strlen(name)};
    sb_collect(collector, cfg, entry, name);
}

static void fixture(const SBConfig *cfg, SBResult *result, SBInventory *inventory) {
    SBCollector collector; sb_collector_init(&collector, true);
    /* A missing size must remain different from an observed zero, even when
     * both sort below known payloads. Nonregular sizes are not payloads. */
    add(&collector, cfg, 2, 1, 2, 0, true, "dir");
    add(&collector, cfg, 3, 2, 1, 10, true, "file");
    add(&collector, cfg, 4, 1, 1, 20, true, "largest");
    add(&collector, cfg, 5, 1, 5, 99, true, "link");
    add(&collector, cfg, 6, 2, 1, 12345, false, "missing\n\033[31m\377");
    add(&collector, cfg, 7, 1, 1, 0, true, "zero");
    *result = (SBResult){.root_device = 42, .root_id = 1, .actual_workers = 1,
        .total_ns = 1234567};
    sb_collect_finalize(&collector, 1, cfg, result, inventory);
    CHECK(result->graph_valid && result->completion == SB_PARTIAL);
    CHECK(result->bytes == 30 && result->unique_bytes == 30 && result->unknown_sizes == 1);
}

static void reject(const SBConfig *cfg, const SBResult *result, const SBInventory *inventory) {
    VantageView view;
    errno = 0;
    CHECK(!vantage_view_init(&view, cfg, result, inventory, true));
    CHECK(errno == EINVAL);
    CHECK(!vantage_view_init(&view, cfg, result, inventory, false));
    CHECK(errno == EINVAL);
    CHECK(!view.offsets && !view.children && !view.files);
    vantage_view_destroy(&view);
}

static void metadata_and_output(void) {
    SBConfig cfg = config(); SBResult result; SBInventory inventory;
    fixture(&cfg, &result, &inventory);
    size_t entry_bytes = inventory.count * sizeof(*inventory.entries);
    SBEntry *original = sb_alloc(entry_bytes); memcpy(original, inventory.entries, entry_bytes);
    VantageView view; CHECK(vantage_view_init(&view, &cfg, &result, &inventory, true));
    CHECK(!memcmp(original, inventory.entries, entry_bytes));
    CHECK(view.root == 6 && view.file_count == 4);
    CHECK(vantage_bytes(&view, view.root) == 30 && vantage_unknown(&view, view.root) == 1);
    CHECK(vantage_bytes(&view, 0) == 10 && vantage_unknown(&view, 0) == 1);
    CHECK(vantage_bytes(&view, 4) == 0 && vantage_unknown(&view, 4) == 1);
    CHECK(vantage_bytes(&view, 5) == 0 && vantage_unknown(&view, 5) == 0);
    CHECK(vantage_bytes(&view, 3) == 0 && vantage_unknown(&view, 3) == 0);
    CHECK(view.children[view.offsets[view.root]] == 2);
    CHECK(view.children[view.offsets[view.root] + 1] == 0);
    CHECK(view.files[0] == 2 && view.files[1] == 1 && view.files[2] == 4 && view.files[3] == 5);
    char *path = vantage_path(&view, 1); CHECK(path && !strcmp(path, "dir/file")); sb_free(path);
    path = vantage_path(&view, view.root); CHECK(path && !strcmp(path, ".")); sb_free(path);
    CHECK(!vantage_path(&view, view.root + 1));

    char *json = capture(&view, true, 10, 8, false);
    CHECK(strstr(json, "\"completion\":\"partial\"") && strstr(json, "\"scan_ns\":1234567"));
    CHECK(strstr(json, "\"path\":\"dir\",\"path_hex\":\"646972\",\"kind\":\"directory\",\"bytes\":10,\"unknown_sizes\":1,\"depth\":1"));
    CHECK(strstr(json, "\"bytes\":null,\"unknown_sizes\":1,\"depth\":2,\"percent\":null"));
    CHECK(strstr(json, "\"path_hex\":\"6469722f6d697373696e670a1b5b33316dff\""));
    CHECK(strstr(json, "\"path\":\"zero\",\"path_hex\":\"7a65726f\",\"kind\":\"file\",\"bytes\":0,\"unknown_sizes\":0"));
    CHECK(strstr(json, "\"kind\":\"symlink\",\"bytes\":0,\"unknown_sizes\":0"));
    CHECK(!strchr(json, '\033'));
    sb_free(json);
    json = capture(&view, true, 1, 8, true);
    CHECK(strstr(json, "\"view\":\"files\"") && strstr(json, "\"path\":\"largest\""));
    CHECK(!strstr(json, "largest_files") && !strstr(json, "\"path\":\"dir/file\""));
    sb_free(json);

    char *plain = capture(&view, false, 10, 8, false);
    CHECK(strstr(plain, "PARTIAL:") && strstr(plain, "lower bounds") && strstr(plain, "unknown"));
    CHECK(strstr(plain, "missing\\n\\x1b[31m\\xff") && !strchr(plain, '\033'));
    CHECK(strstr(plain, "across file paths") && strstr(plain, "across unique file objects"));
    sb_free(plain);

    /* Partial scans can lack whole records without any unknown-size record. */
    result.unknown_sizes = 0;
    plain = capture(&view, false, 1, 1, false);
    CHECK(strstr(plain, "PARTIAL:") && strstr(plain, "lower bounds"));
    sb_free(plain); result.unknown_sizes = 1;
    vantage_view_destroy(&view);

    SBEntry saved = inventory.entries[0];
    inventory.entries[0].parent_index = inventory.count + 1; reject(&cfg, &result, &inventory);
    inventory.entries[0] = saved; inventory.entries[0].parent_id = 999; reject(&cfg, &result, &inventory);
    inventory.entries[0] = saved; inventory.entries[0].parent_index = 0; inventory.entries[0].parent_id = 2;
    reject(&cfg, &result, &inventory);
    inventory.entries[0] = saved; inventory.entries[0].name_offset = inventory.names_size; reject(&cfg, &result, &inventory);
    inventory.entries[0] = saved; inventory.entries[0].name_length = UINT32_MAX; reject(&cfg, &result, &inventory);
    inventory.entries[0] = saved; inventory.entries[0].valid &= ~SB_VALID_KIND; reject(&cfg, &result, &inventory);
    inventory.entries[0] = saved;
    unsigned char first = inventory.names[saved.name_offset];
    inventory.names[saved.name_offset] = '/'; reject(&cfg, &result, &inventory);
    inventory.names[saved.name_offset] = 0; reject(&cfg, &result, &inventory);
    inventory.names[saved.name_offset] = first;
    result.graph_valid = false; reject(&cfg, &result, &inventory); result.graph_valid = true;
    ++result.files; reject(&cfg, &result, &inventory); --result.files;
    CHECK(!memcmp(original, inventory.entries, entry_bytes));
    sb_free(original); sb_inventory_destroy(&inventory);
}

static void empty_graph(void) {
    SBConfig cfg = config();
    SBResult result = {.graph_valid = true, .root_device = 42, .root_id = 1};
    SBInventory inventory = {.task = SB_TREE, .root_device = 42, .root_id = 1};
    VantageView view; CHECK(vantage_view_init(&view, &cfg, &result, &inventory, true));
    CHECK(!view.root && !view.file_count && !view.offsets[0] && !view.offsets[1]);
    char *json = capture(&view, true, 20, 1, false);
    CHECK(strstr(json, "\"items\":[],\"largest_files\":[]")); sb_free(json);
    char *plain = capture(&view, false, 20, 1, false);
    CHECK(strstr(plain, "(empty)") && strstr(plain, "(no regular files)")); sb_free(plain);
    vantage_view_destroy(&view);
}

static void safe_text(void) {
    const unsigned char raw[] = "A\n\033[31m\t\\\177\377\xc2\x85\xe2\x80\xae";
    FILE *stream = tmpfile(); CHECK(stream);
    int columns = vantage_print_text(stream, raw, sizeof(raw) - 1, -1);
    char *text = read_stream(stream);
    CHECK(!strcmp(text, "A\\n\\x1b[31m\\t\\\\\\x7f\\xff\\u0085\\u202e"));
    CHECK(columns == (int)strlen(text)); sb_free(text); CHECK(!fclose(stream));
    for (int cap = 0; cap < 12; ++cap) {
        stream = tmpfile(); CHECK(stream);
        columns = vantage_print_text(stream, raw, sizeof(raw) - 1, cap);
        text = read_stream(stream);
        CHECK(columns >= 0 && columns <= cap && strlen(text) == (size_t)columns);
        CHECK(!strchr(text, '\033') && !strchr(text, '\n') && !strchr(text, '\t'));
        if (cap >= 3) CHECK(strlen(text) >= 3 && !strcmp(text + strlen(text) - 3, "..."));
        sb_free(text); CHECK(!fclose(stream));
    }
    char size[32];
    vantage_size(size, 0); CHECK(!strcmp(size, "0 B"));
    vantage_size(size, 1024); CHECK(!strcmp(size, "1.0 KiB"));
    vantage_size(size, UINT64_MAX); CHECK(!strcmp(size, "16.0 EiB"));
}

static void deep_graph(void) {
    const size_t n = 65536;
    SBConfig cfg = config();
    SBResult result = {.graph_valid = true, .root_device = 42, .root_id = 1,
        .entries = n, .files = 1, .directories = n - 1, .bytes = 7, .unique_bytes = 7};
    SBInventory inventory = {.task = SB_TREE, .root_device = 42, .root_id = 1,
        .count = n, .names_size = 4};
    inventory.entries = sb_calloc(n, sizeof(*inventory.entries));
    inventory.names = sb_alloc(4); memcpy(inventory.names, "f\0d\0", 4);
    /* Leaf first, root child last: a long chain must not require recursion,
     * repeatedly rescan every ancestor, or assume parent-before-child order. */
    for (size_t j = 0; j < n; ++j) {
        inventory.entries[j] = (SBEntry){.device = 42, .object_id = j + 2,
            .parent_id = j + 1 == n ? 1 : j + 3, .parent_index = j + 1,
            .kind = j ? 2 : 1, .valid = SB_REQUIRED | SB_VALID_SIZE,
            .size = j ? 0 : 7, .subtree_bytes = 7, .name_offset = j ? 2 : 0, .name_length = 1};
    }
    VantageView view; CHECK(vantage_view_init(&view, &cfg, &result, &inventory, true));
    CHECK(view.file_count == 1 && view.files[0] == 0);
    CHECK(view.children[view.offsets[view.root]] == n - 1);
    for (size_t j = 1; j < n; ++j)
        CHECK(view.offsets[j + 1] - view.offsets[j] == 1 && view.children[view.offsets[j]] == j - 1);
    char *path = vantage_path(&view, 0); CHECK(path && strlen(path) == n * 2 - 1);
    CHECK(path[0] == 'd' && path[1] == '/' && path[n * 2 - 2] == 'f'); sb_free(path);
    char *json = capture(&view, true, 1, UINT_MAX, false);
    size_t rows = 0;
    for (const char *at = json; (at = strstr(at, "\"kind\":")); ++at) ++rows;
    CHECK(rows == 9); /* Eight tree levels plus the globally ranked leaf. */
    CHECK(strstr(json, "\"depth\":65536,"));
    sb_free(json);
    inventory.entries[n - 1].parent_index = 1;
    inventory.entries[n - 1].parent_id = 3;
    CHECK(!vantage_path(&view, 0) && errno == EINVAL);
    vantage_view_destroy(&view); reject(&cfg, &result, &inventory);
    sb_inventory_destroy(&inventory);
}

static void memory_limit(void) {
    CHECK(!fflush(NULL));
    FILE *errors = tmpfile(); CHECK(errors);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        CHECK(dup2(fileno(errors), STDERR_FILENO) == STDERR_FILENO);
        SBConfig cfg = config();
        SBResult result = {.graph_valid = true};
        SBInventory inventory = {.task = SB_TREE};
        VantageView view;
        sb_mem_limit(1);
        (void)vantage_view_init(&view, &cfg, &result, &inventory, true);
        _exit(99);
    }
    int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 71);
    char *text = read_stream(errors);
    CHECK(strstr(text, "managed memory limit exceeded")); sb_free(text);
    CHECK(!fclose(errors));
}

int main(void) {
    setlocale(LC_CTYPE, "");
    metadata_and_output(); empty_graph(); safe_text(); deep_graph(); memory_limit();
    puts("vantage unit: unknown sizes, malformed/empty/deep graphs, safe text, rankings, and memory cap passed");
    return 0;
}
