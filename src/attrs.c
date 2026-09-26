#include "attrs.h"
#include <string.h>
#include <limits.h>
#ifdef __APPLE__
#include <sys/attr.h>
#define CHECK(local, system) _Static_assert((local) == (system), "Darwin SDK attribute mismatch")
CHECK(VS_A_NAME, ATTR_CMN_NAME);
CHECK(VS_A_DEVID, ATTR_CMN_DEVID);
CHECK(VS_A_OBJTYPE, ATTR_CMN_OBJTYPE);
CHECK(VS_A_FLAGS, ATTR_CMN_FLAGS);
CHECK(VS_A_UUID, ATTR_CMN_UUID);
CHECK(VS_A_FILEID, ATTR_CMN_FILEID);
CHECK(VS_A_ERROR, ATTR_CMN_ERROR);
CHECK(VS_A_RETURNED, ATTR_CMN_RETURNED_ATTRS);
CHECK(VS_D_MOUNTSTATUS, ATTR_DIR_MOUNTSTATUS);
CHECK(VS_F_ALLOCSIZE, ATTR_FILE_ALLOCSIZE);
CHECK(VS_F_DATALENGTH, ATTR_FILE_DATALENGTH);
#endif
VSAttrSpec vs_attr_spec(VSSize size) {
    return (VSAttrSpec){
        .common = VS_A_NAME | VS_A_DEVID | VS_A_OBJTYPE | VS_A_FLAGS | VS_A_FILEID | VS_A_ERROR |
                  VS_A_RETURNED,
        .dir = VS_D_MOUNTSTATUS,
        .file = size == VS_LOGICAL ? VS_F_DATALENGTH : VS_F_ALLOCSIZE,
    };
}

typedef struct {
    const unsigned char *base;
    size_t pos, end;
} Cursor;

static bool take(Cursor *c, void *out, size_t n) {
    if (c->pos > c->end || n > c->end - c->pos) {
        return false;
    }
    if (out) {
        memcpy(out, c->base + c->pos, n);
    }
    c->pos += n;
    return true;
}

static bool take_name(Cursor *c, VSAttrRecord *o) {
    size_t ref = c->pos;
    int32_t relative;
    uint32_t length;
    if (!take(c, &relative, 4) || !take(c, &length, 4)) {
        return false;
    }
    int64_t offset = (int64_t)ref + relative;
    if (!length || offset < (int64_t)c->pos || (uint64_t)offset > c->end ||
        length > c->end - (uint64_t)offset) {
        return false;
    }
    const char *name = (const char *)c->base + (size_t)offset;
    if (name[length - 1] != '\0' || memchr(name, 0, length - 1) || memchr(name, '/', length - 1)) {
        return false;
    }
    o->name = name;
    o->entry.name_length = length - 1;
    c->end = (size_t)offset; /* No fixed-width attribute may overlap the name. */
    return true;
}

/* Decode only the attributes requested by this program. Unknown returned bits
 * are rejected, rather than guessed. 64-bit fields are only four-byte aligned. */
bool vs_attr_decode(const unsigned char *p, size_t available, VSAttrSpec s, VSSize size,
                    VSAttrRecord *o) {
    memset(o, 0, sizeof(*o));
    if (available < 4) {
        return false;
    }
    uint32_t length;
    memcpy(&length, p, 4);
    if (length < 4 || length > available || (length & 3)) {
        return false;
    }
    Cursor c = {p, 4, length};
    uint32_t a[5];
    if (!take(&c, a, sizeof(a))) {
        return false;
    }
    if ((a[0] & ~s.common) || a[1] || (a[2] & ~s.dir) || (a[3] & ~s.file) || a[4]) {
        return false;
    }
    if (!(a[0] & VS_A_RETURNED)) {
        return false;
    }
    uint32_t ac = a[0], ad = a[2], af = a[3];
    if (ad && af) {
        return false;
    }
    if (ac & VS_A_ERROR) {
        if (!take(&c, &o->error, 4)) {
            return false;
        }
        if (o->error) {
            /* Error records may omit type and both type-specific validity masks.
             * Only the bounded error/name prefix is needed by the caller. */
            if ((ac & VS_A_NAME) && !take_name(&c, o)) {
                return false;
            }
            o->record_size = length;
            return true;
        }
    }
    uint32_t pc = s.pack_invalid ? s.common : ac;
    uint32_t pd = s.pack_invalid ? s.dir : ad;
    uint32_t pf = s.pack_invalid ? s.file : af;
    if (!(ac & VS_A_NAME) || !take_name(&c, o)) {
        return false;
    }
    if (pc & VS_A_DEVID) {
        uint32_t dev;
        if (!take(&c, &dev, 4)) {
            return false;
        }
        if (ac & VS_A_DEVID) {
            o->entry.device = dev;
            o->entry.valid |= VS_VALID_DEVICE;
        }
    }
    if (pc & VS_A_OBJTYPE) {
        uint32_t kind;
        if (!take(&c, &kind, 4)) {
            return false;
        }
        if (ac & VS_A_OBJTYPE) {
            if (!kind || kind > 9) {
                return false;
            }
            o->entry.kind = kind;
            o->entry.valid |= VS_VALID_KIND;
        }
    }
    if (pc & VS_A_FLAGS) {
        uint32_t flags;
        if (!take(&c, &flags, 4)) {
            return false;
        }
        if (ac & VS_A_FLAGS) {
            o->flags = flags;
            o->has_flags = true;
        }
    }
    if (pc & VS_A_UUID) {
        if (!take(&c, NULL, 16)) {
            return false;
        }
    }
    if (pc & VS_A_FILEID) {
        uint64_t id;
        if (!take(&c, &id, 8)) {
            return false;
        }
        if (ac & VS_A_FILEID) {
            o->entry.object_id = id;
            o->entry.valid |= VS_VALID_ID;
        }
    }
    if (o->entry.valid & VS_VALID_KIND) {
        if (o->entry.kind == 2) {
            if (af) {
                return false;
            }
            pf = af = 0;
        } else {
            if (ad) {
                return false;
            }
            pd = ad = 0;
        }
    } else if (s.pack_invalid) {
        if (ad) {
            pf = 0;
        } else if (af) {
            pd = 0;
        } else {
            /* No type or type-specific value is valid. Leave the opaque tail
             * uninterpreted; name bounds are known and the caller must enrich. */
            pd = pf = 0;
        }
    }
    if (pd & VS_D_MOUNTSTATUS) {
        uint32_t status;
        if (!take(&c, &status, 4)) {
            return false;
        }
        if (ad & VS_D_MOUNTSTATUS) {
            o->mountstatus = status;
            o->has_mountstatus = true;
        }
    }
    if (pf & VS_F_ALLOCSIZE) {
        uint64_t allocation;
        if (!take(&c, &allocation, 8)) {
            return false;
        }
        if (af & VS_F_ALLOCSIZE) {
            o->allocation = allocation;
            o->has_allocation = true;
        }
    }
    if (pf & VS_F_DATALENGTH) {
        uint64_t logical;
        if (!take(&c, &logical, 8)) {
            return false;
        }
        if (af & VS_F_DATALENGTH) {
            o->logical = logical;
            o->has_logical = true;
        }
    }
    /* Negative signed off_t values and impossible unsigned lengths are invalid. */
    if ((o->has_logical && o->logical > INT64_MAX) ||
        (o->has_allocation && o->allocation > INT64_MAX)) {
        return false;
    }
    if ((o->entry.valid & VS_VALID_KIND) && o->entry.kind != 1) {
        o->entry.size = 0;
        o->entry.valid |= VS_VALID_SIZE;
    } else if (size == VS_LOGICAL && o->has_logical) {
        o->entry.size = o->logical;
        o->entry.valid |= VS_VALID_SIZE;
    } else if (size == VS_ALLOCATED && o->has_allocation) {
        o->entry.size = o->allocation;
        o->entry.valid |= VS_VALID_SIZE;
    }
    o->record_size = length;
    return true;
}
