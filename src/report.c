#include "scanbench.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* Deliberately small JSON reader for result files. It validates the complete
 * document, skips nested values, and extracts only bounded ASCII report keys.
 * It is not a general-purpose JSON or Unicode manipulation library. */
typedef struct { size_t begin, end, after; unsigned char kind; } Token;
typedef struct { const char *s; size_t length, pos, count; Token tokens[1024]; } Json;
static void space(Json *j) { while (j->pos < j->length && strchr(" \t\n\r", j->s[j->pos])) ++j->pos; }
static bool value(Json *j, unsigned depth);
static bool string_token(Json *j) {
    if (j->count == 1024 || j->s[j->pos] != '"') return false;
    size_t index = j->count++; Token *t = j->tokens + index;
    t->kind = 's'; t->begin = ++j->pos;
    while (j->pos < j->length) {
        unsigned char c = (unsigned char)j->s[j->pos++];
        if (c == '"') { t->end = j->pos - 1; t->after = j->count; return true; }
        if (c < 32) return false;
        if (c == '\\') {
            if (j->pos >= j->length) return false;
            c = (unsigned char)j->s[j->pos++];
            if (c == 'u') {
                for (unsigned k = 0; k < 4; ++k)
                    if (j->pos >= j->length || !isxdigit((unsigned char)j->s[j->pos++])) return false;
            } else if (!strchr("\"\\/bfnrt", c)) return false;
        }
    }
    return false;
}
static bool number_token(Json *j) {
    size_t start = j->pos;
    if (j->pos < j->length && j->s[j->pos] == '-') ++j->pos;
    if (j->pos >= j->length) return false;
    if (j->s[j->pos] == '0') ++j->pos;
    else {
        if (j->s[j->pos] < '1' || j->s[j->pos] > '9') return false;
        while (j->pos < j->length && isdigit((unsigned char)j->s[j->pos])) ++j->pos;
    }
    if (j->pos < j->length && j->s[j->pos] == '.') {
        ++j->pos; size_t digits = j->pos;
        while (j->pos < j->length && isdigit((unsigned char)j->s[j->pos])) ++j->pos;
        if (j->pos == digits) return false;
    }
    if (j->pos < j->length && (j->s[j->pos] == 'e' || j->s[j->pos] == 'E')) {
        ++j->pos;
        if (j->pos < j->length && (j->s[j->pos] == '+' || j->s[j->pos] == '-')) ++j->pos;
        size_t digits = j->pos;
        while (j->pos < j->length && isdigit((unsigned char)j->s[j->pos])) ++j->pos;
        if (j->pos == digits) return false;
    }
    j->tokens[j->count] = (Token){start, j->pos, j->count + 1, 'n'}; ++j->count;
    return true;
}
static bool value(Json *j, unsigned depth) {
    space(j); if (depth > 32 || j->pos >= j->length || j->count == 1024) return false;
    char c = j->s[j->pos];
    if (c == '"') return string_token(j);
    if (c == '{' || c == '[') {
        size_t index = j->count++;
        j->tokens[index].begin = j->pos++; j->tokens[index].kind = (unsigned char)c;
        char close = c == '{' ? '}' : ']'; space(j);
        if (j->pos < j->length && j->s[j->pos] == close) ++j->pos;
        else for (;;) {
            space(j);
            if (c == '{') {
                if (j->pos >= j->length || !string_token(j)) return false;
                space(j); if (j->pos >= j->length || j->s[j->pos++] != ':') return false;
            }
            if (!value(j, depth + 1)) return false;
            space(j); if (j->pos >= j->length) return false;
            char next = j->s[j->pos++];
            if (next == close) break;
            if (next != ',') return false;
        }
        j->tokens[index].end = j->pos; j->tokens[index].after = j->count; return true;
    }
    const char *literal = c == 't' ? "true" : c == 'f' ? "false" : c == 'n' ? "null" : NULL;
    if (literal) {
        size_t n = strlen(literal); if (n > j->length - j->pos || memcmp(j->s + j->pos, literal, n)) return false;
        j->tokens[j->count] = (Token){j->pos, j->pos + n, j->count + 1, 'l'};
        ++j->count; j->pos += n; return true;
    }
    return number_token(j);
}
static bool token_is(Json *j, size_t index, const char *s) {
    Token *t = j->tokens + index;
    return t->end - t->begin == strlen(s) && !memcmp(j->s + t->begin, s, strlen(s));
}
static size_t field(Json *j, const char *name) {
    size_t found = SIZE_MAX;
    for (size_t i = 1; i < j->count;) {
        if (i + 1 >= j->count || j->tokens[i].kind != 's') return SIZE_MAX;
        if (token_is(j, i, name)) { if (found != SIZE_MAX) return SIZE_MAX; found = i + 1; }
        i = j->tokens[i + 1].after;
    }
    return found;
}
static bool text(Json *j, const char *key, char *out, size_t capacity) {
    size_t index = field(j, key); if (index == SIZE_MAX) return false;
    Token *t = j->tokens + index; size_t n = t->end - t->begin;
    if (t->kind != 's' || n >= capacity) return false;
    /* Extracted fields are deliberately ASCII. Escaped root/name fields are not used. */
    for (size_t k = 0; k < n; ++k) if ((unsigned char)j->s[t->begin + k] >= 127 || j->s[t->begin + k] == '\\') return false;
    memcpy(out, j->s + t->begin, n); out[n] = 0; return true;
}
static bool integer(Json *j, const char *key, uint64_t *out) {
    size_t index = field(j, key); if (index == SIZE_MAX) return false;
    Token *t = j->tokens + index;
    if (t->kind != 'n' || t->end <= t->begin) return false;
    uint64_t v = 0;
    for (size_t k = t->begin; k < t->end; ++k) {
        unsigned d = (unsigned)(j->s[k] - '0'); if (d > 9 || v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v; return true;
}
typedef struct {
    char session[17], group[17], config[512], method[32], digest[49], task[16], cache[32];
    uint64_t ns, round;
    bool verified;
} Row;
typedef struct { size_t *indexes, count, capacity; Row key; } Group;
typedef struct {
    size_t complete, verified, mismatches, unverified, failed, partial, unsupported;
} StatusCounts;
static bool count_status(StatusCounts *counts, const char *completion, const char *verification) {
    if (strcmp(verification, "verified") && strcmp(verification, "mismatch") && strcmp(verification, "unverified")) return false;
    if (!strcmp(completion, "complete")) {
        ++counts->complete;
        if (!strcmp(verification, "verified")) ++counts->verified;
        else if (!strcmp(verification, "mismatch")) ++counts->mismatches;
        else ++counts->unverified;
    } else if (!strcmp(completion, "failed")) ++counts->failed;
    else if (!strcmp(completion, "partial")) ++counts->partial;
    else if (!strcmp(completion, "unsupported")) ++counts->unsupported;
    else return false;
    return true;
}
static void print_status(const char *label, const StatusCounts *counts) {
    printf("%s: %zu complete (%zu verified, %zu known mismatch, %zu unverified); %zu failed, %zu partial, %zu unsupported.\n",
        label, counts->complete, counts->verified, counts->mismatches, counts->unverified,
        counts->failed, counts->partial, counts->unsupported);
}
static bool same_group(const Row *a, const Row *b) {
    return !strcmp(a->session, b->session) && !strcmp(a->group, b->group) &&
        !strcmp(a->config, b->config) && !strcmp(a->digest, b->digest) && a->verified == b->verified;
}
static int double_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double quantile(double *sorted, size_t n, double q) {
    if (!n) return 0;
    double p = (double)(n - 1) * q; size_t k = (size_t)p;
    return k + 1 < n ? sorted[k] + (sorted[k + 1] - sorted[k]) * (p - (double)k) : sorted[k];
}
static bool comparable_baseline(const Row *a, const Row *b) {
    return b->verified && !strcmp(b->method, "posix") && !strcmp(a->session, b->session) &&
        !strcmp(a->group, b->group) && !strcmp(a->digest, b->digest);
}
static void speedup(const Group *g, const Row *rows, size_t nrows) {
    if (!g->key.verified || !strcmp(g->key.method, "posix")) return;
    double *ratios = sb_alloc(g->count * sizeof(*ratios)); size_t count = 0;
    bool ambiguous = false;
    for (size_t j = 0; j < g->count; ++j) {
        const Row *trial = rows + g->indexes[j]; const Row *baseline = NULL;
        for (size_t k = 0; k < nrows; ++k) if (rows[k].round == trial->round && comparable_baseline(trial, rows + k)) {
            if (baseline) { ambiguous = true; break; } baseline = rows + k;
        }
        if (ambiguous) break;
        if (baseline && trial->ns) ratios[count++] = (double)baseline->ns / (double)trial->ns;
    }
    if (ambiguous) fputs("  Paired speedup unavailable: multiple POSIX baselines for a block.\n", stdout);
    else if (count < 3) fputs("  Paired interval unavailable: fewer than three matching verified baseline pairs.\n", stdout);
    else {
        size_t bootstrap_count = 2000;
        double *estimates = sb_alloc(bootstrap_count * sizeof(*estimates));
        double *sample = sb_alloc(count * sizeof(*sample)); uint64_t rng = UINT64_C(0x6d31535bc4997e81);
        for (size_t b = 0; b < bootstrap_count; ++b) {
            for (size_t k = 0; k < count; ++k) sample[k] = ratios[sb_random(&rng) % count];
            qsort(sample, count, sizeof(*sample), double_cmp); estimates[b] = quantile(sample, count, .5);
        }
        qsort(ratios, count, sizeof(*ratios), double_cmp); qsort(estimates, bootstrap_count, sizeof(*estimates), double_cmp);
        printf("  Paired speedup vs POSIX: %.3fx; 95%% percentile bootstrap [%.3fx, %.3fx], %zu pairs.\n",
            quantile(ratios, count, .5), quantile(estimates, bootstrap_count, .025), quantile(estimates, bootstrap_count, .975), count);
        if (count < 15) fputs("  Small sample: exploratory interval; collect at least 15 measured pairs.\n", stdout);
        sb_free(estimates); sb_free(sample);
    }
    sb_free(ratios);
}
int sb_report(const char *path) {
    bool own_file = strcmp(path, "-") != 0;
    FILE *f = own_file ? fopen(path, "r") : stdin;
    if (!f) { fprintf(stderr, "scanbench: report: %s\n", strerror(errno)); return 1; }
    Row *rows = NULL; size_t count = 0, capacity = 0, lines = 0;
    StatusCounts scans = {0}, references = {0}, preflights = {0}, warmups = {0};
    size_t replay = 0;
    char *line = NULL; size_t line_capacity = 0; ssize_t length; int result = 0;
    while ((length = getline(&line, &line_capacity, f)) >= 0) {
        ++lines; if (length > 1024 * 1024) { result = 1; break; }
        Json json = {.s = line, .length = (size_t)length};
        if (!value(&json, 0)) { result = 1; break; } space(&json);
        if (json.pos != json.length || json.tokens[0].kind != '{') { result = 1; break; }
        char event[32], completion[24], verification[24]; uint64_t schema;
        if (!integer(&json, "schema", &schema) || schema != 1 || !text(&json, "event", event, sizeof(event))) { result = 1; break; }
        if (!strcmp(event, "memory-replay")) { ++replay; continue; }
        if (!text(&json, "completion", completion, sizeof(completion)) || !text(&json, "verification", verification, sizeof(verification))) { result = 1; break; }
        StatusCounts *counts;
        if (!strcmp(event, "trial") || !strcmp(event, "scan")) counts = &scans;
        else if (!strcmp(event, "reference")) counts = &references;
        else if (!strcmp(event, "verification")) counts = &preflights;
        else if (!strcmp(event, "warmup")) counts = &warmups;
        else { result = 1; break; }
        if (!count_status(counts, completion, verification)) { result = 1; break; }
        if (counts != &scans) continue;
        if (strcmp(completion, "complete") || !strcmp(verification, "mismatch")) continue;
        Row row = {0};
#define TEXT(key, member) if (!text(&json, key, row.member, sizeof(row.member))) { result = 1; break; }
        TEXT("session", session); TEXT("group", group); TEXT("config", config); TEXT("method", method);
        TEXT("digest", digest); TEXT("task", task); TEXT("cache", cache);
#undef TEXT
        if (!integer(&json, "total_ns", &row.ns) || !row.ns || !integer(&json, "round", &row.round)) { result = 1; break; }
        row.verified = !strcmp(verification, "verified");
        if (count == 100000) { result = 1; break; }
        if (count == capacity) { capacity = capacity ? capacity * 2 : 64; rows = sb_realloc(rows, capacity * sizeof(*rows)); }
        rows[count++] = row;
    }
    if (ferror(f)) result = 1;
    if (own_file) fclose(f);
    free(line);
    if (result) { fprintf(stderr, "scanbench: malformed, oversized, unsupported-schema, or invalid result at line %zu\n", lines); sb_free(rows); return 1; }
    printf("scanbench report — %zu complete scan trials\n", scans.complete);
    print_status("Scan trials", &scans);
    print_status("Reference records", &references);
    print_status("Preflight records", &preflights);
    print_status("Warmup records", &warmups);
    printf("Memory-replay records excluded: %zu.\n", replay);
    puts("Only complete verified or unverified trials contribute timings; known mismatches are excluded.");
    puts("Groups require identical session, environment/contract group, digest, configuration and verification class.");
    Group *groups = NULL; size_t ngroups = 0;
    for (size_t j = 0; j < count; ++j) {
        size_t k = 0; while (k < ngroups && !same_group(&groups[k].key, rows + j)) ++k;
        if (k == ngroups) { groups = sb_realloc(groups, (ngroups + 1) * sizeof(*groups)); groups[ngroups++] = (Group){.key = rows[j]}; }
        Group *g = groups + k;
        if (g->count == g->capacity) { g->capacity = g->capacity ? g->capacity * 2 : 16; g->indexes = sb_realloc(g->indexes, g->capacity * sizeof(*g->indexes)); }
        g->indexes[g->count++] = j;
    }
    for (size_t k = 0; k < ngroups; ++k) {
        Group *g = groups + k; double *times = sb_alloc(g->count * sizeof(*times));
        for (size_t j = 0; j < g->count; ++j) times[j] = (double)rows[g->indexes[j]].ns / 1e6;
        qsort(times, g->count, sizeof(*times), double_cmp);
        printf("\n%s | %s | %s | %s | n=%zu\n  %s\n", g->key.method, g->key.task, g->key.cache,
            g->key.verified ? "verified" : "UNVERIFIED / exploratory", g->count, g->key.config);
        printf("  Session %s, group %s, digest %.16s…\n  Median %.3f ms; IQR [%.3f, %.3f] ms.\n",
            g->key.session, g->key.group, g->key.digest, quantile(times, g->count, .5), quantile(times, g->count, .25), quantile(times, g->count, .75));
        speedup(g, rows, count); sb_free(times); sb_free(g->indexes);
    }
    if (!count) puts("No eligible scan timings. Inspect original JSON for capability failures or use bench to collect trials.");
    sb_free(groups); sb_free(rows); return 0;
}
