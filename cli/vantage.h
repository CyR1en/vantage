#ifndef VANTAGE_H
#define VANTAGE_H

#include "scan.h"

#define VANTAGE_VERSION "0.1.0"

typedef struct {
    const VSConfig *config;
    const VSResult *result;
    const VSInventory *inventory;
    size_t root;
    size_t *offsets;
    size_t *children;
    size_t *files;
    size_t file_count;
    uint64_t index_ns;
} VantageView;

bool vantage_view_init(VantageView *view, const VSConfig *config, const VSResult *result,
                       const VSInventory *inventory, bool build_index);
void vantage_view_destroy(VantageView *view);
uint64_t vantage_bytes(const VantageView *view, size_t entry);
uint64_t vantage_unknown(const VantageView *view, size_t entry);
char *vantage_path(const VantageView *view, size_t entry);
void vantage_size(char out[32], uint64_t bytes);
int vantage_print_text(FILE *out, const unsigned char *text, size_t length, int max_columns);
void vantage_report(const VantageView *view, size_t top, unsigned depth, bool files_only);
void vantage_json(const VantageView *view, size_t top, unsigned depth, bool files_only);
void vantage_export(const VantageView *view, FILE *out);
int vantage_tui(const VantageView *view, bool files_only);

#endif
