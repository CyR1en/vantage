# Primary API references

These references informed the interface use and limitations. Production code is
original; the project does not vendor Apple, HFS, or third-party scanner source.
Published source is not a promise that a particular macOS build has identical
implementation behavior. The installed SDK and native verification remain decisive.

* Darwin bulk enumeration manual: https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/man/man2/getattrlistbulk.2
* Attribute layout and packing manual: https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/man/man2/getattrlist.2
* Catalog search/continuation manual: https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/man/man2/searchfs.2
* Public attribute constants, structs, and supported-ID notes: https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/sys/attr.h
* `getattrlistbulk` and `getattrlistat` prototypes/availability: https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/sys/unistd.h
* XNU attribute path selection and packing: https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/vfs/vfs_attrlist.c
* HFS search matching behavior: https://raw.githubusercontent.com/apple-oss-distributions/hfs/main/core/hfs_search.c
* HFS type-conditional catalog attribute packing: https://raw.githubusercontent.com/apple-oss-distributions/hfs/main/core/hfs_attrlist.c
* Existing open-source catalog-search example (not included): https://github.com/sveinbjornt/searchfs

For the unimplemented research tracks:

* Apple File System Reference: https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf
* FSEvents programming guide: https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/FSEvents_ProgGuide/UsingtheFSEventsFramework/UsingtheFSEventsFramework.html

Key choices: request 64-bit FILEID/PARENTID, never legacy 32-bit IDs; do not put
RETURNED_ATTRS into `searchfs`; decode four-byte-aligned fields with checked copies;
process EAGAIN batches and discard prior attempts on EBUSY; preserve different
logical/allocation contracts; never equate multiple disjoint search result sets
with disjoint physical I/O.
