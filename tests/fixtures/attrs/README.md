# Captured Darwin attribute records

`captured.h` embeds 48 records in 16 batches copied byte-for-byte from
`normal-records/*.bin` and `error-records/*.bin` beside this file.
It is input captured from `getattrlistbulk`, independent of the synthetic encoder
in `tests/wire.h`. Embedding the bytes lets the unit executable run from any
working directory without loading fixture files at runtime.

The captures were made on September 15, 2026 with macOS 27.0 build 26A428,
Darwin 27.0.0 / xnu-13432.1.9, arm64, APFS, SDK 27.0, and Apple clang
21.0.0. The request was `sb_attr_spec` for the logical tree task, crossed with
packing enabled/disabled, UUID requested/not requested, and object type
requested/omitted. Each request used a newly opened directory descriptor.
The captured batches, checksums, and embedded header are included here;
the original syscall harness and full hex logs are outside the source distribution.

The normal fixture contains a seven-byte regular file, a directory, a symlink to
the file, and a FIFO. The error fixture contains a file and directory temporarily
given the ACL `everyone deny readattr`. Requests with UUID or omitted object type
produce EACCES error records. Those error records have no valid object type,
directory attributes, or file attributes; packed records still have a name after
the invalid placeholders. The default request is native-eligible. No driver-path
tracing was performed, so these captures do not establish whether that individual
call used VNOP or an ENOTSUP fallback.

On this host, successful packed records contain a returned zero-valued ERROR
field; successful unpacked records omit it. The parser still keys its presence
on the returned mask. Synthetic regression cases exercise a packed record with
ERROR absent too, without claiming that configuration was observed here.

The public implementation consulted is Apple XNU
[f6217f891ac0bb64f3d375211650a4c1ff8ca1ea, xnu-12377.1.9](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/vfs/vfs_attrlist.c),
which predates the running kernel. Its section selection, error-record construction,
and UUID/omitted-type fallback conditions informed the investigation; captured
bytes are the direct evidence for this host. The
[getattrlistbulk manual](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/man/man2/getattrlistbulk.2)
documents the returned validity mask and per-entry error field.

`tests/unit.c` checks every record's name, identity where valid, type validity,
file/directory fields, error, and complete batch consumption. It rejects every
truncated prefix, exercises malformed masks and names, and mutates all capture
configurations under sanitizers. A successful packed record with unknown type
and no valid type-specific fields retains only bounded common metadata; the
caller must enrich it. The parser does not infer a type or size from zeroes,
padding, or an ambiguous fixed-width tail.

The SHA-256 of each original batch is recorded in `SHA256SUMS`. To verify the
original binaries from the repository root:

```sh
cd tests/fixtures/attrs
shasum -a 256 -c SHA256SUMS
```

Array names in `captured.h` replace the source path separators and dashes with
underscores and remove `records` and `.bin`. The arrays are plain hexadecimal
serialization; regenerating them must copy the captured bytes, never call the
synthetic wire encoder or the production decoder.
