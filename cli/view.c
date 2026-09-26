#include "vantage.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    size_t entry, parent;
    uint64_t bytes, object_id, parent_id;
    const unsigned char *name;
    uint32_t length;
} Ranked;

uint64_t vantage_bytes(const VantageView *v, size_t entry) {
    if (entry == v->root) {
        return v->result->bytes;
    }
    if (entry >= v->inventory->count) {
        return 0;
    }
    const VSEntry *e = v->inventory->entries + entry;
    return e->kind == 2                                 ? e->subtree_bytes
           : e->kind == 1 && (e->valid & VS_VALID_SIZE) ? e->size
                                                        : 0;
}

uint64_t vantage_unknown(const VantageView *v, size_t entry) {
    if (entry == v->root) {
        return v->result->unknown_sizes;
    }
    if (entry >= v->inventory->count) {
        return 0;
    }
    const VSEntry *e = v->inventory->entries + entry;
    return e->kind == 2 ? e->subtree_unknown : e->kind == 1 && !(e->valid & VS_VALID_SIZE) ? 1 : 0;
}

static int rank_compare(const void *left, const void *right) {
    const Ranked *a = left, *b = right;
    if (a->bytes != b->bytes) {
        return a->bytes > b->bytes ? -1 : 1;
    }
    size_t length = a->length < b->length ? a->length : b->length;
    int order = memcmp(a->name, b->name, length);
    if (order) {
        return order;
    }
    if (a->length != b->length) {
        return a->length < b->length ? -1 : 1;
    }
    if (a->object_id != b->object_id) {
        return a->object_id < b->object_id ? -1 : 1;
    }
    if (a->parent_id != b->parent_id) {
        return a->parent_id < b->parent_id ? -1 : 1;
    }
    return a->entry < b->entry ? -1 : a->entry > b->entry;
}

static int siblings_compare(const void *left, const void *right) {
    const Ranked *a = left, *b = right;
    if (a->parent != b->parent) {
        return a->parent < b->parent ? -1 : 1;
    }
    return rank_compare(left, right);
}

void vantage_view_destroy(VantageView *v) {
    vs_free(v->offsets);
    vs_free(v->children);
    vs_free(v->files);
    memset(v, 0, sizeof(*v));
}

bool vantage_view_init(VantageView *v, const VSConfig *cfg, const VSResult *r,
                       const VSInventory *inv, bool build_index) {
    uint64_t started = vs_now_ns();
    memset(v, 0, sizeof(*v));
    if (!r->graph_valid || r->entries != inv->count || r->root_device != inv->root_device ||
        r->root_id != inv->root_id || inv->count > SIZE_MAX - 2 ||
        (inv->count && (!inv->entries || !inv->names))) {
        errno = EINVAL;
        return false;
    }
    size_t n = inv->count;
    v->config = cfg;
    v->result = r;
    v->inventory = inv;
    v->root = n;
    Ranked *ranked = NULL;
    unsigned char *state = vs_calloc(n + 1, sizeof(*state));
    state[n] = 2;
    size_t directories = 0;
    for (size_t j = 0; j < n; ++j) {
        const VSEntry *e = inv->entries + j;
        if ((e->valid & VS_REQUIRED) != VS_REQUIRED || e->parent_index > n ||
            e->name_offset >= inv->names_size || !e->name_length ||
            e->name_length >= inv->names_size - e->name_offset) {
            goto invalid;
        }
        const unsigned char *name = inv->names + e->name_offset;
        if (name[e->name_length] || memchr(name, 0, e->name_length) ||
            memchr(name, '/', e->name_length) || (e->name_length == 1 && name[0] == '.') ||
            (e->name_length == 2 && name[0] == '.' && name[1] == '.')) {
            goto invalid;
        }
        size_t parent = (size_t)e->parent_index;
        if (parent == n) {
            if (e->device != r->root_device || e->parent_id != r->root_id) {
                goto invalid;
            }
        } else {
            const VSEntry *p = inv->entries + parent;
            if (p->kind != 2 || p->device != e->device || p->object_id != e->parent_id) {
                goto invalid;
            }
        }
        if (e->kind == 2) {
            if (e->device == r->root_device && e->object_id == r->root_id) {
                goto invalid;
            }
            ++directories;
        } else if (e->kind == 1) {
            ++v->file_count;
        }
    }
    if (v->file_count != r->files || directories != r->directories) {
        goto invalid;
    }
    /* Each entry is visited at most twice, including chains discovered before
     * their parents. A malformed graph cannot make path/navigation loops hang. */
    for (size_t j = 0; j < n; ++j) {
        size_t at = j;
        while (!state[at]) {
            state[at] = 1;
            at = (size_t)inv->entries[at].parent_index;
        }
        if (state[at] == 1) {
            goto invalid;
        }
        at = j;
        while (state[at] == 1) {
            state[at] = 2;
            at = (size_t)inv->entries[at].parent_index;
        }
    }
    /* Binary exports use inventory order. Validate them identically, without
     * allocating or sorting the terminal browser's ranking indexes. */
    if (!build_index) {
        goto done;
    }
    v->offsets = vs_calloc(n + 2, sizeof(*v->offsets));
    v->children = vs_calloc(n, sizeof(*v->children));
    ranked = vs_calloc(n, sizeof(*ranked));
    for (size_t j = 0; j < n; ++j) {
        const VSEntry *e = inv->entries + j;
        size_t parent = (size_t)e->parent_index;
        ++v->offsets[parent + 1];
        ranked[j] = (Ranked){j,
                             parent,
                             vantage_bytes(v, j),
                             e->object_id,
                             e->parent_id,
                             inv->names + e->name_offset,
                             e->name_length};
    }
    for (size_t j = 1; j < n + 2; ++j) {
        v->offsets[j] += v->offsets[j - 1];
    }
    if (n) {
        qsort(ranked, n, sizeof(*ranked), siblings_compare);
    }
    for (size_t j = 0; j < n; ++j) {
        v->children[j] = ranked[j].entry;
    }
    v->files = vs_calloc(v->file_count, sizeof(*v->files));
    size_t count = 0;
    for (size_t j = 0; j < n; ++j) {
        if (inv->entries[ranked[j].entry].kind == 1) {
            ranked[count++] = ranked[j];
        }
    }
    if (count) {
        qsort(ranked, count, sizeof(*ranked), rank_compare);
    }
    for (size_t j = 0; j < count; ++j) {
        v->files[j] = ranked[j].entry;
    }
done:
    vs_free(state);
    vs_free(ranked);
    v->index_ns = vs_now_ns() - started;
    return true;
invalid:
    vs_free(state);
    vs_free(ranked);
    vantage_view_destroy(v);
    errno = EINVAL;
    return false;
}

char *vantage_path(const VantageView *v, size_t entry) {
    if (entry == v->root) {
        char *path = vs_alloc(2);
        memcpy(path, ".", 2);
        return path;
    }
    size_t at = entry, length = 0, steps = 0;
    while (at != v->root) {
        if (at >= v->inventory->count || ++steps > v->inventory->count) {
            errno = EINVAL;
            return NULL;
        }
        const VSEntry *e = v->inventory->entries + at;
        if (e->parent_index > v->root ||
            (e->parent_index < v->root && v->inventory->entries[e->parent_index].kind != 2)) {
            errno = EINVAL;
            return NULL;
        }
        size_t add = e->name_length + (steps > 1 ? (size_t)1 : 0);
        if (add >= SIZE_MAX - length) {
            errno = EOVERFLOW;
            return NULL;
        }
        length += add;
        at = (size_t)e->parent_index;
    }
    char *path = vs_alloc(length + 1);
    path[length] = 0;
    at = entry;
    size_t end = length;
    while (at != v->root) {
        const VSEntry *e = v->inventory->entries + at;
        end -= e->name_length;
        memcpy(path + end, v->inventory->names + e->name_offset, e->name_length);
        at = (size_t)e->parent_index;
        if (at != v->root) {
            path[--end] = '/';
        }
    }
    return path;
}

void vantage_size(char out[32], uint64_t bytes) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
    if (bytes < 1024) {
        snprintf(out, 32, "%" PRIu64 " B", bytes);
        return;
    }
    double value = (double)bytes;
    unsigned unit = 0;
    while (value >= 1024 && unit < 6) {
        value /= 1024;
        ++unit;
    }
    snprintf(out, 32, "%.1f %s", value, units[unit]);
}

typedef struct {
    size_t consumed, length;
    int columns;
    unsigned char text[16];
} TextPart;

static TextPart text_part(const unsigned char *s, size_t available) {
    TextPart p = {.consumed = 1};
    unsigned char c = s[0];
    if (c >= 32 && c < 127 && c != '\\') {
        p.text[0] = c;
        p.length = 1;
        p.columns = 1;
        return p;
    }
    if (c == '\\' || c == '\n' || c == '\r' || c == '\t') {
        p.text[0] = '\\';
        p.text[1] = c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : '\\';
        p.length = 2;
        p.columns = 2;
        return p;
    }
    unsigned n = c >= 0xc2 && c <= 0xdf   ? 2
                 : c >= 0xe0 && c <= 0xef ? 3
                 : c >= 0xf0 && c <= 0xf4 ? 4
                                          : 0;
    uint32_t code = n ? c & (0x7fu >> n) : 0;
    bool valid = n && n <= available;
    for (unsigned j = 1; valid && j < n; ++j) {
        if ((s[j] & 0xc0) != 0x80) {
            valid = false;
        } else {
            code = (code << 6) | (s[j] & 0x3f);
        }
    }
    valid = valid &&
            code >= (n == 2   ? 0x80u
                     : n == 3 ? 0x800u
                              : 0x10000u) &&
            code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
    if (valid) {
        p.consumed = n;
        int width = wcwidth((wchar_t)code);
        bool control = code < 0xa0 || code == 0x061c || (code >= 0x200e && code <= 0x200f) ||
                       (code >= 0x2028 && code <= 0x202e) || (code >= 0x2066 && code <= 0x206f);
        if (!control && width > 0) {
            memcpy(p.text, s, n);
            p.length = n;
            p.columns = width;
            return p;
        }
        int length = snprintf((char *)p.text, sizeof(p.text),
                              code <= 0xffff ? "\\u%04" PRIx32 : "\\U%08" PRIx32, code);
        p.length = (size_t)length;
        p.columns = length;
        return p;
    }
    p.length = 4;
    p.columns = 4;
    snprintf((char *)p.text, sizeof(p.text), "\\x%02x", c);
    return p;
}

int vantage_print_text(FILE *out, const unsigned char *text, size_t length, int max_columns) {
    if (!max_columns || !length) {
        return 0;
    }
    bool truncated = false;
    if (max_columns >= 0) {
        int columns = 0;
        for (size_t at = 0; at < length;) {
            TextPart p = text_part(text + at, length - at);
            if (p.columns > max_columns - columns) {
                truncated = true;
                break;
            }
            columns += p.columns;
            at += p.consumed;
        }
    }
    int dots = truncated ? (max_columns < 3 ? max_columns : 3) : 0;
    int limit = max_columns < 0 ? INT_MAX : max_columns - dots, columns = 0;
    for (size_t at = 0; at < length;) {
        TextPart p = text_part(text + at, length - at);
        if (p.columns > limit - columns) {
            break;
        }
        fwrite(p.text, 1, p.length, out);
        columns += p.columns;
        at += p.consumed;
    }
    for (int j = 0; j < dots; ++j) {
        fputc('.', out);
    }
    return columns + dots;
}

static bool unknown_file(const VantageView *v, size_t entry) {
    const VSEntry *e = v->inventory->entries + entry;
    return e->kind == 1 && !(e->valid & VS_VALID_SIZE);
}

static double percent(const VantageView *v, size_t entry, size_t parent) {
    uint64_t denominator = vantage_bytes(v, parent);
    return denominator
               ? (double)((long double)vantage_bytes(v, entry) * 100 / (long double)denominator)
               : 0;
}

typedef void (*Emit)(const VantageView *, size_t, size_t, unsigned, void *);

static void tree_rows(const VantageView *v, size_t parent, size_t top, unsigned depth,
                      unsigned limit, Emit emit, void *context) {
    size_t begin = v->offsets[parent], count = v->offsets[parent + 1] - begin;
    if (count > top) {
        count = top;
    }
    for (size_t j = 0; j < count; ++j) {
        size_t entry = v->children[begin + j];
        emit(v, entry, parent, depth, context);
        if (depth < limit && v->inventory->entries[entry].kind == 2) {
            tree_rows(v, entry, top, depth + 1, limit, emit, context);
        }
    }
}

static void file_rows(const VantageView *v, size_t top, Emit emit, void *context) {
    size_t count = top < v->file_count ? top : v->file_count;
    for (size_t j = 0; j < count; ++j) {
        emit(v, v->files[j], v->root, 1, context);
    }
}

static void report_row(const VantageView *v, size_t entry, size_t parent, unsigned depth,
                       void *context) {
    bool full_path = *(const bool *)context;
    const VSEntry *e = v->inventory->entries + entry;
    char size[32], bar[13];
    vantage_size(size, vantage_bytes(v, entry));
    double share = percent(v, entry, parent);
    if (unknown_file(v, entry)) {
        memcpy(size, "unknown", 8);
        memset(bar, '?', 12);
    } else {
        int filled = share >= 100 ? 12 : (int)(share * .12 + .5);
        for (int j = 0; j < 12; ++j) {
            bar[j] = j < filled ? '#' : ' ';
        }
    }
    bar[12] = 0;
    printf("%11s%c ", size, vantage_unknown(v, entry) && !unknown_file(v, entry) ? '*' : ' ');
    if (unknown_file(v, entry)) {
        fputs("     ? ", stdout);
    } else {
        printf("%6.1f%% ", share);
    }
    printf("[%s] ", bar);
    for (unsigned j = 1; j < depth; ++j) {
        fputs("  ", stdout);
    }
    char *path = full_path ? vantage_path(v, entry) : NULL;
    const unsigned char *name =
        path ? (const unsigned char *)path : v->inventory->names + e->name_offset;
    vantage_print_text(stdout, name, path ? strlen(path) : e->name_length, -1);
    if (e->kind == 2) {
        fputc('/', stdout);
    } else if (e->kind == 5) {
        fputs(" [symlink]", stdout);
    } else if (e->kind != 1) {
        fputs(" [other]", stdout);
    }
    fputc('\n', stdout);
    vs_free(path);
}

void vantage_report(const VantageView *v, size_t top, unsigned depth, bool files_only) {
    if (!depth) {
        depth = 1;
    }
    if (depth > 8) {
        depth = 8;
    }
    const VSResult *r = v->result;
    printf("vantage - %s usage\nRoot: ",
           v->config->size == VS_LOGICAL ? "logical data-fork" : "allocated file-fork");
    vantage_print_text(stdout, (const unsigned char *)v->config->root, strlen(v->config->root), -1);
    printf("\nStatus: %s", vs_completion_name(r->completion));
    if (r->reason[0]) {
        fputs(" - ", stdout);
        vantage_print_text(stdout, (const unsigned char *)r->reason, strlen(r->reason), -1);
    }
    char total[32], unique[32];
    vantage_size(total, r->bytes);
    vantage_size(unique, r->unique_bytes);
    printf("\nUsage: %s across file paths; %s across unique file objects\n", total, unique);
    printf("Entries: %" PRIu64 " | Files: %" PRIu64 " | Directories: %" PRIu64 "\n", r->entries,
           r->files, r->directories);
    printf("Scan: %.2f ms | Index: %.2f ms | %s / %u workers\n", (double)r->total_ns / 1e6,
           (double)v->index_ns / 1e6, vs_method_name(v->config->method), r->actual_workers);
    puts("Hard-link aliases count in path totals; unique totals count each file object once.");
    if (r->completion != VS_COMPLETE) {
        puts(
            "PARTIAL: known sizes are lower bounds; inaccessible or changed entries may be missing.");
    }
    if (r->unknown_sizes) {
        printf("* Known bytes only; %" PRIu64 " file path sizes remain unknown.\n",
               r->unknown_sizes);
    }
    bool full_path = false;
    if (!files_only) {
        printf("\nLargest entries (top %zu per directory, depth %u; %% of parent)\n", top, depth);
        if (v->offsets[v->root] == v->offsets[v->root + 1]) {
            puts("  (empty)");
        } else {
            tree_rows(v, v->root, top, 1, depth, report_row, &full_path);
        }
    }
    printf("\nLargest files (top %zu across all directories; %% of total)\n", top);
    full_path = true;
    if (!v->file_count) {
        puts("  (no regular files)");
    } else {
        file_rows(v, top, report_row, &full_path);
    }
}

typedef struct {
    bool first, files;
} JsonRows;

static void json_row(const VantageView *v, size_t entry, size_t parent, unsigned depth,
                     void *context) {
    JsonRows *rows = context;
    if (!rows->first) {
        fputc(',', stdout);
    }
    rows->first = false;
    const VSEntry *e = v->inventory->entries + entry;
    char *path = vantage_path(v, entry);
    if (rows->files && path) {
        depth = 1;
        for (const char *p = path; *p; ++p) {
            if (*p == '/' && depth < UINT_MAX) {
                ++depth;
            }
        }
    }
    fputs("{\"path\":", stdout);
    vs_json_string(stdout, path ? path : "");
    fputs(",\"path_hex\":", stdout);
    vs_json_hex(stdout, (const unsigned char *)(path ? path : ""), path ? strlen(path) : 0);
    fputs(",\"kind\":", stdout);
    vs_json_string(stdout, e->kind == 1   ? "file"
                           : e->kind == 2 ? "directory"
                           : e->kind == 5 ? "symlink"
                                          : "other");
    fputs(",\"bytes\":", stdout);
    if (unknown_file(v, entry)) {
        fputs("null", stdout);
    } else {
        printf("%" PRIu64, vantage_bytes(v, entry));
    }
    printf(",\"unknown_sizes\":%" PRIu64 ",\"depth\":%u,\"percent\":", vantage_unknown(v, entry),
           depth);
    if (unknown_file(v, entry)) {
        fputs("null", stdout);
    } else {
        printf("%.6f", percent(v, entry, parent));
    }
    fputc('}', stdout);
    vs_free(path);
}

void vantage_json(const VantageView *v, size_t top, unsigned depth, bool files_only) {
    if (!depth) {
        depth = 1;
    }
    if (depth > 8) {
        depth = 8;
    }
    const VSResult *r = v->result;
    fputs("{\"schema\":1,\"root\":", stdout);
    vs_json_string(stdout, v->config->root);
    fputs(",\"root_hex\":", stdout);
    vs_json_hex(stdout, (const unsigned char *)v->config->root, strlen(v->config->root));
    fputs(",\"method\":", stdout);
    vs_json_string(stdout, vs_method_name(v->config->method));
    printf(",\"workers\":%u,\"buffer_bytes\":%" PRIu64 ",\"size\":", r->actual_workers,
           v->config->buffer_size);
    vs_json_string(stdout, vs_size_name(v->config->size));
    fputs(",\"completion\":", stdout);
    vs_json_string(stdout, vs_completion_name(r->completion));
    fputs(",\"reason\":", stdout);
    vs_json_string(stdout, r->reason);
    printf(",\"graph_valid\":%s,\"total_bytes\":%" PRIu64 ",\"unique_bytes\":%" PRIu64
           ",\"unknown_sizes\":%" PRIu64 ",\"scan_ns\":%" PRIu64 ",\"index_ns\":%" PRIu64
           ",\"entries\":%" PRIu64 ",\"files\":%" PRIu64 ",\"directories\":%" PRIu64 ",\"view\":",
           r->graph_valid ? "true" : "false", r->bytes, r->unique_bytes, r->unknown_sizes,
           r->total_ns, v->index_ns, r->entries, r->files, r->directories);
    vs_json_string(stdout, files_only ? "files" : "tree");
    fputs(",\"items\":[", stdout);
    JsonRows output = {.first = true, .files = files_only};
    if (files_only) {
        file_rows(v, top, json_row, &output);
    } else {
        tree_rows(v, v->root, top, 1, depth, json_row, &output);
    }
    fputc(']', stdout);
    if (!files_only) {
        fputs(",\"largest_files\":[", stdout);
        output = (JsonRows){.first = true, .files = true};
        file_rows(v, top, json_row, &output);
        fputc(']', stdout);
    }
    fputs("}\n", stdout);
}
