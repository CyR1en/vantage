#include "scanbench.h"
#include <inttypes.h>
#include <string.h>

/* UTF-8 stays UTF-8. Invalid byte sequences are escaped to U+00xx for display;
 * root_hex and manifest name_hex remain the lossless source of truth. */
static unsigned utf8_length(const unsigned char *s) {
    unsigned n = s[0] >= 0xc2 && s[0] <= 0xdf ? 2 : s[0] >= 0xe0 && s[0] <= 0xef ? 3 : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
    if (!n) return 0;
    for (unsigned j = 1; j < n; ++j) if (!s[j] || (s[j] & 0xc0) != 0x80) return 0;
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) || (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90)) return 0;
    return n;
}
void sb_json_string(FILE *f, const char *str) {
    const unsigned char *s = (const unsigned char *)str; fputc('"', f);
    while (*s) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); ++s; }
        else if (c < 32) { fprintf(f, "\\u%04x", c); ++s; }
        else if (c < 128) { fputc(c, f); ++s; }
        else { unsigned n = utf8_length(s); if (n) { fwrite(s, 1, n, f); s += n; } else { fprintf(f, "\\u%04x", c); ++s; } }
    }
    fputc('"', f);
}
void sb_json_hex(FILE *f, const unsigned char *s, size_t n) {
    static const char hex[] = "0123456789abcdef"; fputc('"', f);
    for (size_t j = 0; j < n; ++j) { fputc(hex[s[j] >> 4], f); fputc(hex[s[j] & 15], f); }
    fputc('"', f);
}
void sb_result_json(FILE *f, const SBConfig *c, const SBResult *r, const char *event) {
    char key[512]; sb_config_key(c, key, sizeof(key));
    fprintf(f, "{\"schema\":%d,\"version\":\"%s\",\"event\":", SB_SCHEMA, SB_VERSION); sb_json_string(f, event);
    fprintf(f, ",\"session\":\"%016" PRIx64 "\",\"group\":\"%016" PRIx64 "\",\"config\":", c->session, r->group_id); sb_json_string(f, key);
    fputs(",\"method\":", f); sb_json_string(f, sb_method_name(c->method));
    fputs(",\"root\":", f); sb_json_string(f, c->root);
    fputs(",\"root_hex\":", f); sb_json_hex(f, (const unsigned char *)c->root, strlen(c->root));
    fputs(",\"task\":", f); sb_json_string(f, sb_task_name(c->task));
    fputs(",\"size_contract\":", f); sb_json_string(f, c->task == SB_ENUMERATE ? "none" : c->size == SB_LOGICAL ? "regular-data-fork-logical" : "regular-all-forks-ATTR_FILE_ALLOCSIZE");
    fputs(",\"cache\":", f); sb_json_string(f, sb_cache_name(c->cache));
    fputs(",\"consistency\":", f); sb_json_string(f, sb_consistency_name(c->consistency));
    fputs(",\"consistency_basis\":\"caller-asserted\",\"completion\":", f); sb_json_string(f, sb_completion_name(r->completion));
    fputs(",\"verification\":", f); sb_json_string(f, sb_verification_name(r->verification));
    fputs(",\"verification_basis\":", f); sb_json_string(f, r->verification_basis);
    fputs(",\"coverage\":", f); sb_json_string(f, r->counters.permission_errors || (c->method >= SB_CATALOG && r->verification != SB_VERIFIED) ? "accessible-subset" : r->completion == SB_COMPLETE ? "requested-scope" : "incomplete");
    fputs(",\"reason\":", f); sb_json_string(f, r->reason);
    fprintf(f, ",\"errno\":%d,\"round\":%u,\"config_index\":%u,\"workers\":%u,\"actual_workers\":%u,\"buffer_bytes\":%" PRIu64,
        r->error_code, c->round, c->config_index, c->workers, r->actual_workers, c->buffer_size);
    fprintf(f, ",\"seed\":\"%" PRIu64 "\",\"memory_limit_bytes\":%" PRIu64 ",\"timeout_ns\":%" PRIu64,
        c->seed, c->memory_limit, c->timeout_ns);
    fputs(",\"order\":", f); sb_json_string(f, sb_order_name(c->order));
    fprintf(f, ",\"frontier\":%u,\"id_shift\":%u,\"fd_limit\":%u,\"queue_limit\":%u,\"catalog_partitions\":%u,\"catalog_retry_budget\":%u",
        c->frontier, c->id_shift, c->fd_limit, c->queue_limit, c->catalog_partitions, c->catalog_retries);
    fprintf(f, ",\"pack_invalid\":%s,\"skip_empty\":%s,\"adaptive\":%s,\"diagnostic\":%s,\"allow_volume_scan\":%s",
        c->pack_invalid ? "true" : "false", c->skip_empty ? "true" : "false", c->adaptive ? "true" : "false", c->diagnostic ? "true" : "false", c->allow_volume_scan ? "true" : "false");
    fprintf(f, ",\"extra_entrycount\":%s,\"extra_linkcount\":%s,\"extra_uuid\":%s,\"omit_objtype\":%s",
        c->extra_entrycount ? "true" : "false", c->extra_linkcount ? "true" : "false", c->extra_uuid ? "true" : "false", c->omit_objtype ? "true" : "false");
    fputs(",\"reduction\":", f); sb_json_string(f, c->reduce == SB_REDUCE_SORT ? "sort" : "hash");
#define R(name) fprintf(f, ",\"" #name "\":%" PRIu64, r->name)
    R(total_ns); R(setup_ns); R(scan_ns); R(finalize_ns); R(process_wall_ns);
    R(user_ns); R(system_ns); R(max_rss_bytes); R(managed_peak_bytes); R(timestamp_ns);
    R(entries); R(files); R(directories); R(symlinks); R(other); R(unique_objects); R(unique_files);
    R(bytes); R(unique_bytes); R(unknown_sizes); R(source_entries); R(graph_root_bytes);
#undef R
    fprintf(f, ",\"root_device\":\"%" PRIu64 "\",\"root_id\":\"%" PRIu64 "\",\"digest\":\"%016" PRIx64 "%016" PRIx64 "%016" PRIx64 "\"",
        r->root_device, r->root_id, r->digest_a, r->digest_b, r->digest_c);
    fprintf(f, ",\"graph_valid\":%s,\"volume_root\":%s,\"readonly_mount\":%s,\"uid\":%u,\"gid\":%u,\"cpus\":%u",
        c->task == SB_TREE ? (r->graph_valid ? "true" : "false") : "null", r->volume_root ? "true" : "false", r->readonly_mount ? "true" : "false", r->uid, r->gid, r->cpus);
    fputs(",\"os\":", f); sb_json_string(f, r->os);
    fputs(",\"machine\":", f); sb_json_string(f, r->machine);
    fputs(",\"hardware_model\":", f); sb_json_string(f, r->hardware_model);
    fputs(",\"product_version\":", f); sb_json_string(f, r->product_version);
    fprintf(f, ",\"physical_memory_bytes\":%" PRIu64, r->physical_memory_bytes);
    fputs(",\"filesystem\":", f); sb_json_string(f, r->filesystem);
    fputs(",\"build\":", f); sb_json_string(f, r->build);
    fputs(",\"scope_note\":", f); sb_json_string(f, r->scope_note);
    fputs(",\"physical_read_bytes\":null,\"thermal_state\":null,\"power_mode\":null,\"full_disk_access\":null,\"qos\":\"inherited-default\",\"cpu_accounting\":\"process-lifetime\",\"counters\":{", f);
    fprintf(f, "\"readdir_calls\":%" PRIu64, r->counters.readdir_calls);
#define C(name) fprintf(f, ",\"" #name "\":%" PRIu64, r->counters.name)
    C(stat_calls); C(bulk_calls); C(search_calls); C(opens); C(reopens); C(enrichment_calls);
    C(batches); C(batch_entries); C(max_batch); C(metadata_bytes); C(excluded_boundaries);
    C(empty_opens_avoided); C(errors); C(permission_errors); C(vanished); C(identity_races);
    C(malformed_records); C(duplicates); C(missing_parents); C(unknown_attributes); C(queue_peak);
    C(catalog_restarts); C(adaptive_changes); C(search_ns); C(traversal_ns);
#undef C
    fputs("}}\n", f);
}
