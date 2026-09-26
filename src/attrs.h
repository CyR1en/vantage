#ifndef VS_ATTRS_H
#define VS_ATTRS_H
#include "scan.h"
/* Private names for documented Darwin wire constants, also usable in portable
 * parser tests. Actual macOS builds statically check these against the SDK. */
#define VS_A_NAME UINT32_C(0x00000001)
#define VS_A_DEVID UINT32_C(0x00000002)
#define VS_A_OBJTYPE UINT32_C(0x00000008)
#define VS_A_FLAGS UINT32_C(0x00040000)
#define VS_A_UUID UINT32_C(0x00800000)
#define VS_A_FILEID UINT32_C(0x02000000)
#define VS_A_ERROR UINT32_C(0x20000000)
#define VS_A_RETURNED UINT32_C(0x80000000)
#define VS_D_MOUNTSTATUS UINT32_C(0x00000004)
#define VS_F_ALLOCSIZE UINT32_C(0x00000004)
#define VS_F_DATALENGTH UINT32_C(0x00000200)
#define VS_F_FIRMLINK UINT32_C(0x00800000)

typedef struct {
    uint32_t common, dir, file;
    bool pack_invalid;
} VSAttrSpec;

typedef struct {
    VSEntry entry;
    const char *name;
    uint32_t error, flags, mountstatus;
    uint64_t allocation, logical;
    bool has_flags, has_mountstatus, has_logical, has_allocation;
    size_t record_size;
} VSAttrRecord;

VSAttrSpec vs_attr_spec(VSSize size);
bool vs_attr_decode(const unsigned char *p, size_t available, VSAttrSpec spec, VSSize size,
                    VSAttrRecord *out);
#endif
