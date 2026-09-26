#ifndef VS_DARWIN_TEST_SHIM_H
#define VS_DARWIN_TEST_SHIM_H
#ifdef __APPLE__
#include <sys/attr.h>
#include <unistd.h>
#else
#include <stdint.h>
#include <sys/time.h>
#define ATTR_BIT_MAP_COUNT 5
#define ATTR_CMN_NAME UINT32_C(1)
#define ATTR_CMN_RETURNED_ATTRS UINT32_C(0x80000000)
#define ATTR_CMN_DEVID UINT32_C(2)
#define ATTR_CMN_OBJTYPE UINT32_C(8)
#define ATTR_FILE_ALLOCSIZE UINT32_C(4)
#define FSOPT_NOFOLLOW UINT32_C(1)
#define FSOPT_PACK_INVAL_ATTRS UINT32_C(8)
#define ATTR_CMN_FILEID UINT32_C(0x02000000)

struct attrlist {
    uint16_t bitmapcount, reserved;
    uint32_t commonattr, volattr, dirattr, fileattr, forkattr;
};

int getattrlistbulk(int, void *, void *, size_t, uint64_t);
int getattrlistat(int, const char *, void *, void *, size_t, unsigned long);
#endif
#endif
