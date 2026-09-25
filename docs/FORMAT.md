# Result and inventory formats

## JSON Lines (schema 1)

One JSON object per line. Event types are `scan`, `reference`, `verification`,
`warmup`, `trial`, `probe`, and `memory-replay`. Probe/replay have their own smaller
schemas; a probe is not a scan result. The report reads scan/benchmark result files,
not a mixture containing probe objects.

Scan rows contain session/group/config keys, method/root/task/size/cache contracts,
consistency assertions, completion/verification/coverage, all experiment settings,
phase and process timing in integer nanoseconds, process-lifetime CPU/RSS, managed
memory peak, counts/byte totals, root identity, digest, build/environment, and
counters. Model/product-version/physical-memory metadata is available on Darwin;
blank or zero means unavailable elsewhere. Unknown environmental measurements
such as physical reads and thermal state are `null`.

Object/device IDs, seed, session, and digest are strings to preserve full 64-bit
identity through consumers that use floating-point numbers. Nanosecond and byte
fields are JSON integers; consumers must preserve 64-bit integer precision.
`root_hex` is lossless raw pathname bytes. `root` is display text; invalid UTF-8
bytes are escaped rather than emitted as invalid JSON.

`completion`: complete, partial, unsupported, failed.
`verification`: verified, mismatch, unverified.
A verified result names its basis: exact POSIX manifest or exact preflight plus
trial digest. The reference itself is not self-certified as an independent oracle.
Known preflight mismatches use `exact-preflight-mismatch` in later warmups and
trials; a later scan failure retains both the mismatch and the failure reason.
`coverage` remains conservative for unverified catalog scans. These fields are
independent; “complete” alone is not proof of full requested-scope correctness.

Counters include API calls, opens/reopens, batches/entries/metadata bytes, required
enrichment, errors, exclusions, unknown fields, queue peak, restarts, and adaptive
changes. API durations are zero unless diagnostic timing is enabled. Data forks,
resource forks, allocation, and reclaimable storage are not conflated.

## Exact JSON inventory

`scan --manifest-json FILE` writes one normalized entry per line, sorted by device,
parent, raw name, and identity/metadata. Fields: `device`, `id`, `parent`, `kind`,
`valid`, `name_hex`, `size`, `subtree_bytes`, `subtree_unknown`. A missing requested
size is JSON null, not zero. The root anchor is omitted; use the companion scan
result for root identity and task/size context.

Kind values mirror Darwin vtype: regular 1, directory 2, block 3, character 4,
symlink 5, socket 6, FIFO 7; other values are retained but not regular-file bytes.
Valid bits: size=1, identity=2, parent=4, kind=8, device=16.

## Binary inventory: SBINV001

This is normalized inventory, **not raw Darwin record capture**. All stored
integers are little-endian. There is no compression or checksum; exact comparison
and digest belong to the caller. The reader checks bounds, layout, names, and
resource limits. Parent array indexes are reconstructed; raw identities are kept.

Header (48 bytes):

| Offset | Type | Value |
|---:|---|---|
| 0 | 8 bytes | ASCII `SBINV001` |
| 8 | uint64 | entry count |
| 16 | uint64 | name arena size, including one terminator per entry |
| 24 | uint64 | root device |
| 32 | uint64 | root object ID |
| 40 | uint32 | task: enumerate=0, usage=1, tree=2 |
| 44 | uint32 | size: logical=0, allocated=1 |

Each entry is a fixed 60-byte record followed immediately by `name_length` raw
bytes (no stored terminator). The next entry begins immediately after those bytes.

| Offset | Type | Field |
|---:|---|---|
| 0 | uint64 | device |
| 8 | uint64 | object ID |
| 16 | uint64 | parent object ID |
| 24 | uint64 | requested size; meaningful only if valid bit set |
| 32 | uint64 | subtree bytes |
| 40 | uint64 | subtree unknown-size count |
| 48 | uint32 | kind |
| 52 | uint32 | validity bits |
| 56 | uint32 | raw name byte length |

The parent/child subprocess channel also transmits a private C struct. It is
only an internal same-executable protocol, not a stable persistent ABI.
