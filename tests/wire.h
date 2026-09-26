#ifndef VS_TEST_WIRE_H
#define VS_TEST_WIRE_H
#include "attrs.h"
#include <string.h>

/* Synthetic provider for state-machine tests; captured.h tests the kernel ABI. */
static void wire32(unsigned char *b, size_t *p, uint32_t v) {
    memcpy(b + *p, &v, 4);
    *p += 4;
}

static void wire64(unsigned char *b, size_t *p, uint64_t v) {
    memcpy(b + *p, &v, 8);
    *p += 8;
}

static size_t wire_encode(unsigned char *b, VSAttrSpec s, VSEntry e, const char *name,
                          uint32_t actual_common, uint32_t actual_dir, uint32_t actual_file,
                          uint32_t error, uint32_t flags) {
    size_t p = 4, ref = 0;
    memset(b, 0, 512);
    uint32_t ac = actual_common, ad = actual_dir, af = actual_file;
    wire32(b, &p, ac);
    wire32(b, &p, 0);
    wire32(b, &p, ad);
    wire32(b, &p, af);
    wire32(b, &p, 0);
    uint32_t pc = s.pack_invalid ? s.common : ac;
    uint32_t pd = s.pack_invalid ? (e.kind == 2 ? s.dir : 0) : ad;
    uint32_t pf = s.pack_invalid ? (e.kind == 2 ? 0 : s.file) : af;
    if (ac & VS_A_ERROR) {
        wire32(b, &p, error);
    }
    if (pc & VS_A_NAME) {
        ref = p;
        p += 8;
    }
    if (pc & VS_A_DEVID) {
        wire32(b, &p, ac & VS_A_DEVID ? (uint32_t)e.device : 0);
    }
    if (pc & VS_A_OBJTYPE) {
        wire32(b, &p, ac & VS_A_OBJTYPE ? e.kind : 0);
    }
    if (pc & VS_A_FLAGS) {
        wire32(b, &p, ac & VS_A_FLAGS ? flags : 0);
    }
    if (pc & VS_A_UUID) {
        p += 16;
    }
    if (pc & VS_A_FILEID) {
        wire64(b, &p, ac & VS_A_FILEID ? e.object_id : 0);
    }
    if (pd & VS_D_MOUNTSTATUS) {
        wire32(b, &p, 0);
    }
    if (pf & VS_F_ALLOCSIZE) {
        wire64(b, &p, af & VS_F_ALLOCSIZE ? e.size * 2 : 0);
    }
    if (pf & VS_F_DATALENGTH) {
        wire64(b, &p, af & VS_F_DATALENGTH ? e.size : 0);
    }
    if ((ac & VS_A_NAME) && ref) {
        size_t length = strlen(name) + 1, at = ref;
        wire32(b, &at, (uint32_t)(p - ref));
        wire32(b, &at, (uint32_t)length);
        memcpy(b + p, name, length);
        p += length;
    }
    p = (p + 3) & ~(size_t)3;
    size_t zero = 0;
    wire32(b, &zero, (uint32_t)p);
    return p;
}
#endif
