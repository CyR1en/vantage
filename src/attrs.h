#ifndef SB_ATTRS_H
#define SB_ATTRS_H
#include "scanbench.h"
/* Private names for documented Darwin wire constants, also usable in portable
 * parser tests. Actual macOS builds statically check these against the SDK. */
#define SB_A_NAME UINT32_C(0x00000001)
#define SB_A_DEVID UINT32_C(0x00000002)
#define SB_A_OBJTYPE UINT32_C(0x00000008)
#define SB_A_FLAGS UINT32_C(0x00040000)
#define SB_A_UUID UINT32_C(0x00800000)
#define SB_A_FILEID UINT32_C(0x02000000)
#define SB_A_PARENTID UINT32_C(0x04000000)
#define SB_A_ERROR UINT32_C(0x20000000)
#define SB_A_RETURNED UINT32_C(0x80000000)
#define SB_D_LINKCOUNT UINT32_C(0x00000001)
#define SB_D_ENTRYCOUNT UINT32_C(0x00000002)
#define SB_D_MOUNTSTATUS UINT32_C(0x00000004)
#define SB_F_LINKCOUNT UINT32_C(0x00000001)
#define SB_F_ALLOCSIZE UINT32_C(0x00000004)
#define SB_F_DATALENGTH UINT32_C(0x00000200)
#define SB_F_FIRMLINK UINT32_C(0x00800000)

typedef struct { uint32_t common, dir, file; bool returned, pack_invalid; } SBAttrSpec;
typedef struct {
    SBEntry entry;
    const char *name;
    uint32_t error, flags, mountstatus, entrycount;
    uint64_t allocation, logical;
    bool has_entrycount, has_flags, has_mountstatus, has_logical, has_allocation;
    size_t record_size;
} SBAttrRecord;
SBAttrSpec sb_attr_spec(const SBConfig *c, bool catalog);
bool sb_attr_decode(const unsigned char *p, size_t available,
                    SBAttrSpec spec, SBSize size, SBAttrRecord *out);
#endif
