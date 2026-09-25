#include "attrs.h"
#include <string.h>
#include <limits.h>
#ifdef __APPLE__
#include <sys/attr.h>
#define CHECK(local, system) _Static_assert((local) == (system), "Darwin SDK attribute mismatch")
CHECK(SB_A_NAME, ATTR_CMN_NAME); CHECK(SB_A_DEVID, ATTR_CMN_DEVID);
CHECK(SB_A_OBJTYPE, ATTR_CMN_OBJTYPE); CHECK(SB_A_FLAGS, ATTR_CMN_FLAGS);
CHECK(SB_A_UUID, ATTR_CMN_UUID); CHECK(SB_A_FILEID, ATTR_CMN_FILEID);
CHECK(SB_A_PARENTID, ATTR_CMN_PARENTID); CHECK(SB_A_ERROR, ATTR_CMN_ERROR);
CHECK(SB_A_RETURNED, ATTR_CMN_RETURNED_ATTRS);
CHECK(SB_D_ENTRYCOUNT, ATTR_DIR_ENTRYCOUNT); CHECK(SB_D_MOUNTSTATUS, ATTR_DIR_MOUNTSTATUS);
CHECK(SB_F_ALLOCSIZE, ATTR_FILE_ALLOCSIZE); CHECK(SB_F_DATALENGTH, ATTR_FILE_DATALENGTH);
#endif
SBAttrSpec sb_attr_spec(const SBConfig *c, bool catalog) {
    SBAttrSpec s = {0};
    s.common = SB_A_NAME | SB_A_DEVID | SB_A_OBJTYPE | SB_A_FLAGS | SB_A_FILEID;
    if (catalog) s.common |= SB_A_PARENTID;
    else { s.common |= SB_A_ERROR | SB_A_RETURNED; s.returned = true; s.pack_invalid = c->pack_invalid; }
    if (c->omit_objtype && !catalog) s.common &= ~SB_A_OBJTYPE;
    if (c->extra_uuid) s.common |= SB_A_UUID;
    s.dir = SB_D_MOUNTSTATUS;
    if (c->skip_empty || c->extra_entrycount) s.dir |= SB_D_ENTRYCOUNT;
    if (c->extra_linkcount) { s.dir |= SB_D_LINKCOUNT; s.file |= SB_F_LINKCOUNT; }
    if (c->task != SB_ENUMERATE) s.file |= c->size == SB_LOGICAL ? SB_F_DATALENGTH : SB_F_ALLOCSIZE;
    return s;
}
typedef struct { const unsigned char *base; size_t pos, end; } Cursor;
static bool take(Cursor *c, void *out, size_t n) {
    if (c->pos > c->end || n > c->end - c->pos) return false;
    if (out) memcpy(out, c->base + c->pos, n);
    c->pos += n; return true;
}
static bool take_name(Cursor *c, SBAttrRecord *o) {
    size_t ref = c->pos;
    int32_t relative; uint32_t length;
    if (!take(c, &relative, 4) || !take(c, &length, 4)) return false;
    int64_t offset = (int64_t)ref + relative;
    if (!length || offset < (int64_t)c->pos || (uint64_t)offset > c->end || length > c->end - (uint64_t)offset) return false;
    const char *name = (const char *)c->base + (size_t)offset;
    if (name[length - 1] != '\0' || memchr(name, 0, length - 1) || memchr(name, '/', length - 1)) return false;
    o->name = name; o->entry.name_length = length - 1;
    c->end = (size_t)offset; /* No fixed-width attribute may overlap the name. */
    return true;
}
/* Decode only the attributes requested by this program. Unknown returned bits
 * are rejected, rather than guessed. 64-bit fields are only four-byte aligned. */
bool sb_attr_decode(const unsigned char *p, size_t available, SBAttrSpec s,
                    SBSize size, SBAttrRecord *o) {
    memset(o, 0, sizeof(*o));
    if (available < 4) return false;
    uint32_t length; memcpy(&length, p, 4);
    if (length < 4 || length > available || (length & 3)) return false;
    Cursor c = {p, 4, length};
    uint32_t ac = s.common, ad = s.dir, af = s.file;
    if (s.returned) {
        uint32_t a[5]; if (!take(&c, a, sizeof(a))) return false;
        if ((a[0] & ~s.common) || a[1] || (a[2] & ~s.dir) || (a[3] & ~s.file) || a[4]) return false;
        if (!(a[0] & SB_A_RETURNED)) return false;
        ac = a[0]; ad = a[2]; af = a[3];
    }
    if (s.returned && ad && af) return false;
    if (ac & SB_A_ERROR) {
        if (!take(&c, &o->error, 4)) return false;
        if (o->error) {
            /* Error records may omit type and both type-specific validity masks.
             * Only the bounded error/name prefix is needed by the caller. */
            if ((ac & SB_A_NAME) && !take_name(&c, o)) return false;
            o->record_size = length; return true;
        }
    }
    uint32_t pc = s.pack_invalid ? s.common : ac;
    uint32_t pd = s.pack_invalid ? s.dir : ad;
    uint32_t pf = s.pack_invalid ? s.file : af;
    if (!(ac & SB_A_NAME) || !take_name(&c, o)) return false;
    if (pc & SB_A_DEVID) {
        uint32_t dev; if (!take(&c, &dev, 4)) return false;
        if (ac & SB_A_DEVID) { o->entry.device = dev; o->entry.valid |= SB_VALID_DEVICE; }
    }
    if (pc & SB_A_OBJTYPE) {
        uint32_t kind; if (!take(&c, &kind, 4)) return false;
        if (ac & SB_A_OBJTYPE) { if (!kind || kind > 9) return false; o->entry.kind = kind; o->entry.valid |= SB_VALID_KIND; }
    }
    if (pc & SB_A_FLAGS) {
        uint32_t flags; if (!take(&c, &flags, 4)) return false;
        if (ac & SB_A_FLAGS) { o->flags = flags; o->has_flags = true; }
    }
    if (pc & SB_A_UUID) { if (!take(&c, NULL, 16)) return false; }
    if (pc & SB_A_FILEID) {
        uint64_t id; if (!take(&c, &id, 8)) return false;
        if (ac & SB_A_FILEID) { o->entry.object_id = id; o->entry.valid |= SB_VALID_ID; }
    }
    if (pc & SB_A_PARENTID) {
        uint64_t id; if (!take(&c, &id, 8)) return false;
        if (ac & SB_A_PARENTID) { o->entry.parent_id = id; o->entry.valid |= SB_VALID_PARENT; }
    }
    if (o->entry.valid & SB_VALID_KIND) {
        if (o->entry.kind == 2) {
            if (s.returned && af) return false;
            pf = af = 0;
        } else {
            if (s.returned && ad) return false;
            pd = ad = 0;
        }
    } else if (!s.returned) return false;
    else if (s.pack_invalid) {
        if (ad) pf = 0;
        else if (af) pd = 0;
        else {
            /* No type or type-specific value is valid. Leave the opaque tail
             * uninterpreted; name bounds are known and the caller must enrich. */
            pd = pf = 0;
        }
    }
    if (pd & SB_D_LINKCOUNT) { if (!take(&c, NULL, 4)) return false; }
    if (pd & SB_D_ENTRYCOUNT) {
        uint32_t count; if (!take(&c, &count, 4)) return false;
        if (ad & SB_D_ENTRYCOUNT) { o->entrycount = count; o->has_entrycount = true; }
    }
    if (pd & SB_D_MOUNTSTATUS) {
        uint32_t status; if (!take(&c, &status, 4)) return false;
        if (ad & SB_D_MOUNTSTATUS) { o->mountstatus = status; o->has_mountstatus = true; }
    }
    if (pf & SB_F_LINKCOUNT) { if (!take(&c, NULL, 4)) return false; }
    if (pf & SB_F_ALLOCSIZE) {
        uint64_t allocation; if (!take(&c, &allocation, 8)) return false;
        if (af & SB_F_ALLOCSIZE) { o->allocation = allocation; o->has_allocation = true; }
    }
    if (pf & SB_F_DATALENGTH) {
        uint64_t logical; if (!take(&c, &logical, 8)) return false;
        if (af & SB_F_DATALENGTH) { o->logical = logical; o->has_logical = true; }
    }
    /* Negative signed off_t values and impossible unsigned lengths are invalid. */
    if ((o->has_logical && o->logical > INT64_MAX) || (o->has_allocation && o->allocation > INT64_MAX)) return false;
    if ((o->entry.valid & SB_VALID_KIND) && o->entry.kind != 1) { o->entry.size = 0; o->entry.valid |= SB_VALID_SIZE; }
    else if (size == SB_LOGICAL && o->has_logical) { o->entry.size = o->logical; o->entry.valid |= SB_VALID_SIZE; }
    else if (size == SB_ALLOCATED && o->has_allocation) { o->entry.size = o->allocation; o->entry.valid |= SB_VALID_SIZE; }
    o->record_size = length; return true;
}
