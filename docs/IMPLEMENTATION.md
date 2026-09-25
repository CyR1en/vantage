# Implementation coverage and deliberate limits

This file describes the implemented algorithms, architecture, and limits.

## Delivered

| Area | Delivered behavior |
|---|---|
| POSIX baseline | Serial and parallel descriptor-relative walkers; no symlink following. |
| Bulk metadata | Native macOS API, reusable buffers, returned-field validation, placeholder packing, timed enrichment. |
| Parallel traversal | Shared bounded worker queue, constant-time FIFO ring and DFS/random choices, bounded ID-band candidate frontier. |
| Descriptor safety | Prefetched directory descriptors within a budget; component-wise `openat` reopening with identity checks. |
| Queue exhaustion | Explicit failed run with diagnostic; no silent dropping and no producer/consumer deadlock. |
| Catalog search | Full-volume, resumable `searchfs`, strict 64-bit IDs, checked parent paths and no-follow enrichment for actual types and canonical sizes; all lookup costs timed. |
| Catalog continuation | Partial-result EAGAIN, discard-and-restart on EBUSY, bounded retries charged to timing. |
| Catalog pipeline | Two buffers, single cursor owner, producer/consumer overlap, cancellation cleanup. |
| Catalog partitions | Opt-in disjoint 1/2/4 ID predicates; exact union verification required. |
| H1 attributes | Extra count/link/UUID fields, object-type negative control, fixed/variable packing, enrichment counters. |
| H2 ID bands | Nearest numerical band in a bounded shared frontier; native-order/random/FIFO controls. |
| H3 empty directories | Skip opening a valid zero-count directory only with caller-asserted immutable consistency. |
| H4 adaptive workers | Coarse throughput/queue-depth controller with hysteresis and recorded changes. |
| H5 bookkeeping | Worker-local append-only collectors and name arenas; post-scan sort or hash unique-object reduction. |
| H6 catalog partitioning | Implemented as a separately named speculative backend, omitted from `all`. |
| H7 CPU isolation | Normalized-inventory collector replay, separately labeled and timed. |
| Inventory correctness | Exact sorted manifests; multiplicity-sensitive three-accumulator digest in each trial; graph validation. |
| Benchmark orchestration | Fresh exec child per scan trial, seeded block randomization, warmups, first-pass restrictions, JSONL. |
| Reporting | Median and IQR; paired within-block POSIX speedup; deterministic 2000-resample percentile bootstrap. |
| Fixtures/tests | Independent Python filesystem/allocation oracle, captured native records, mutation tests, bulk/catalog providers, benchmark failure injection, and harness failure tests. |
| Native harness | Read-only APFS contract matrix and repeated multi-corpus study; release/sanitizer checks in CI. |
| Terminal browser | Retained tree navigation, ranked reports, JSON, and SVX1 exports using the shared scanner. |
| Mac app | SwiftUI browsing, saved scans, FSEvents refresh, cache publication, helper cancellation, and confirmed file removal. |

## Architecture and limits

The scheduler is a **bounded shared queue**, not per-worker work-stealing deques.
The ID-band implementation searches a bounded candidate set; it does not bucket
physical extents. The adaptive controller uses throughput and queue occupancy;
it does not measure a per-worker kernel contention model or select an optimal
configuration in advance.

The test suite exercises the production packed-record decoder, including 48
independently captured native records, truncated prefixes, and 100,000 mutated
inputs, but the `replay` command replays **normalized inventory entries**.
It does not capture and replay raw `getattrlistbulk`/`searchfs` response batches.
Consequently its numbers cannot isolate Darwin wire-parser cost by themselves.

The prototype retains object summaries even for the streaming tasks, because
unique-object accounting and hard-link consistency checking are required. It
is not a constant-memory “sum file lengths” program. Tree mode additionally
retains entries/names and temporarily allocates reduction and graph structures.

Graph construction and finalization are single-threaded. The hash-reduction
variant changes only unique-object reduction, not every graph data structure.
No SIMD, hand-written assembly, PGO, affinity pinning, or asynchronous kernel I/O
is enabled by default.

`searchfs` must supply the required search records and 64-bit identities; an
unsupported search is not replaced with a POSIX traversal. Selected entries are
then enriched through checked descriptor-relative lookups because driver type
and length fields alone do not establish the promised semantics. APFS/HFS+/other
versions may differ. Exact verification remains essential, especially for hard
links, mount boundaries, and firmlinks. See [native validation](VALIDATION.md#native-volume-validation).

## Not implemented

* `scanbench-apfs`, `raw-tree`, and `raw-gather`: no raw-device or APFS-image parser.
* A benchmark-CLI `refresh` method or background-maintained index. Vantage.app
  has its own saved scans and FSEvents refresh; these are separate from fresh-scan benchmarks.
* APFS maintained-directory aggregates or exclusive clone/snapshot space accounting.
* Automated cold-cache preparation, snapshot creation, privileges/TCC repair,
  storage I/O tracing, thermal measurement, or attribution of physical disk reads.
* A statistical winner-promotion command or cross-session paired confidence intervals.

There are no fake benchmark rows or executable placeholders for these missing
features. Unsupported method/task names are rejected.

## Read-only scope

The scanner does not intentionally alter the scanned namespace or read regular
file payloads. Metadata queries can still affect caches, access accounting, or
filesystem-provider behavior; “read-only scanner” is not a promise that an OS
will perform no writes or cloud-provider interactions. The optional generator
and APFS fixture script intentionally create their **own** test data.

Do not treat live scans as atomic snapshots. Identity checks catch some rename
and replacement races, but no algorithm here guarantees a coherent transaction
view of a mutating mounted namespace. `immutable` and `quiescent` are explicitly
caller assertions, not detected guarantees.
