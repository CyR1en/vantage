#ifndef SB_TEST_WIRE_H
#define SB_TEST_WIRE_H
#include "attrs.h"
#include <string.h>
/* Synthetic provider for state-machine tests; captured.h tests the kernel ABI. */
static void wire32(unsigned char *b, size_t *p, uint32_t v) { memcpy(b + *p, &v, 4); *p += 4; }
static void wire64(unsigned char *b, size_t *p, uint64_t v) { memcpy(b + *p, &v, 8); *p += 8; }
static size_t wire_encode(unsigned char *b, SBAttrSpec s, SBEntry e,
                           const char *name, uint32_t actual_common, uint32_t actual_dir,
                           uint32_t actual_file, uint32_t error, uint32_t flags, uint32_t child_count) {
    size_t p = 4, ref = 0;
    memset(b, 0, 512);
    uint32_t ac = s.returned ? actual_common : s.common;
    uint32_t ad = s.returned ? actual_dir : e.kind == 2 ? s.dir : 0;
    uint32_t af = s.returned ? actual_file : e.kind == 2 ? 0 : s.file;
    if (s.returned) { wire32(b, &p, ac); wire32(b, &p, 0); wire32(b, &p, ad); wire32(b, &p, af); wire32(b, &p, 0); }
    uint32_t pc = s.pack_invalid ? s.common : ac;
    uint32_t pd = s.pack_invalid ? (e.kind == 2 ? s.dir : 0) : ad;
    uint32_t pf = s.pack_invalid ? (e.kind == 2 ? 0 : s.file) : af;
    if (ac & SB_A_ERROR) wire32(b, &p, error);
    if (pc & SB_A_NAME) { ref = p; p += 8; }
    if (pc & SB_A_DEVID) wire32(b, &p, ac & SB_A_DEVID ? (uint32_t)e.device : 0);
    if (pc & SB_A_OBJTYPE) wire32(b, &p, ac & SB_A_OBJTYPE ? e.kind : 0);
    if (pc & SB_A_FLAGS) wire32(b, &p, ac & SB_A_FLAGS ? flags : 0);
    if (pc & SB_A_UUID) p += 16;
    if (pc & SB_A_FILEID) wire64(b, &p, ac & SB_A_FILEID ? e.object_id : 0);
    if (pc & SB_A_PARENTID) wire64(b, &p, ac & SB_A_PARENTID ? e.parent_id : 0);
    if (pd & SB_D_LINKCOUNT) wire32(b, &p, ad & SB_D_LINKCOUNT ? 1 : 0);
    if (pd & SB_D_ENTRYCOUNT) wire32(b, &p, ad & SB_D_ENTRYCOUNT ? child_count : 0);
    if (pd & SB_D_MOUNTSTATUS) wire32(b, &p, 0);
    if (pf & SB_F_LINKCOUNT) wire32(b, &p, af & SB_F_LINKCOUNT ? 1 : 0);
    if (pf & SB_F_ALLOCSIZE) wire64(b, &p, af & SB_F_ALLOCSIZE ? e.size * 2 : 0);
    if (pf & SB_F_DATALENGTH) wire64(b, &p, af & SB_F_DATALENGTH ? e.size : 0);
    if ((ac & SB_A_NAME) && ref) {
        size_t length = strlen(name) + 1, at = ref;
        wire32(b, &at, (uint32_t)(p - ref)); wire32(b, &at, (uint32_t)length);
        memcpy(b + p, name, length); p += length;
    }
    p = (p + 3) & ~(size_t)3; size_t zero = 0; wire32(b, &zero, (uint32_t)p); return p;
}
#endif
