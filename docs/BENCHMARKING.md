# Benchmark protocol

## Compare equivalent work

Use a dedicated filesystem/corpus, preferably an immutable image. Hold task,
size contract, scope, accessible namespace, power state, and build constant.
A `tree` result is not interchangeable with a filename search or size-only sum.
`allocated` is reported filesystem allocation, not exclusive/reclaimable storage.

`verify` runs a conservative POSIX reference and compares complete, normalized
entry manifests for each requested configuration. `bench` normally performs the
same preflight, then compares each timed run's digest and totals to that reference.
Counts or byte totals alone do not certify equality. The digest is a regression
check after exact preflight, not a cryptographic proof. Test fixtures also have
an independently implemented Python oracle.

A complete result can still be **unverified**. Failed, partial, unsupported, and
mismatching results remain in JSON. The report excludes those from eligible
scan timings; complete-but-unverified scans are labeled exploratory and receive
no paired speedup claim. Missing attributes are not zero-byte entries.

An incomplete exact reference aborts the benchmark after retaining its failure
row. A known preflight mismatch remains a mismatch in subsequent warmups and
trials, even if a later digest matches. Reports count measured verified,
mismatching, unverified, failed, partial, and unsupported rows separately from
reference, preflight, and warmup outcomes.

## Cache labels

`bench` defaults to warm: exact preflight and one warmup per configuration,
followed by 15 measured rounds. This warms metadata intentionally, but does not
prove that the working set fits in RAM. A warm benchmark must enable either
preflight or at least one warmup.

A single `scan` defaults to `first-pass`, meaning cache residency is unknown,
not that the machine is cold. `probe`, `realpath` resolution, fixture creation,
Finder, indexing, and other processes may touch metadata before a scan. Root
path canonicalization is outside the scan timer and can warm ancestor metadata.

`first-pass`, `reboot-prepared`, and `remounted-fixture` benchmark modes require
**one configuration, one round, zero warmups, and no preflight**. The runner does
not reboot, remount, clear caches, or establish the caller's preparation.

```sh
./build/scanbench bench /Volumes/Fixture --methods bulk \
  --cache first-pass --rounds 1 --warmup 0 --verify none \
  --out ../first-pass-bulk.jsonl
```

Prepare each candidate independently. A then B against the same newly mounted
image does not give two first passes. Do not verify immediately before a cold
measurement. None of `purge`, memory pressure, or no-cache flags is implemented
as a magic “all metadata is cold” switch.

## Timing boundaries

Each scan/reference/preflight/warmup runs in a freshly executed child process.
`total_ns` covers root opening and environment collection, backend setup, thread
startup, enumeration, required enrichment, retries, normalization, digesting,
unique-object reduction, graph construction, and cleanup. `setup_ns + scan_ns +
finalize_ns == total_ns`. Thread startup is within the scan phase. Finalization
is the remainder, including dispatch/teardown gaps.

Catalog scan time includes whole-volume enumeration, subtree selection, and
identity-checked no-follow metadata enrichment of every selected entry. This
enrichment establishes actual file types and canonical requested sizes; corrected
catalog timings cannot be compared as equivalent work to older results that
trusted incorrect driver-reported types.

`process_wall_ns` additionally includes process launch, result transfer, optional
manifest export, and exit. It is not interchangeable with `total_ns`, especially
for preflight calls that return exact manifests. Manifest serialization and
exact comparison are outside the scan timer; the same lightweight digest remains
inside every comparable trial.

User/system CPU and RSS are **process-lifetime** measurements, not phase-delta
metrics. Managed-memory peak covers the program's tracked allocations, not
thread stacks, allocator overhead, libc storage, or the benchmark parent.

The scan deadline is checked between batches/work items. It cannot interrupt
an individual stuck filesystem syscall. The parent waits for a result header
with an additional 30-second allowance, then kills the worker if necessary.
`--timeout 0` disables the internal scan deadline, but the parent retains an
approximately one-hour result-header watchdog. Manifest reception is not a
separately timed syscall watchdog.

## Ordering and statistics

Each block contains every eligible configuration once, in seeded randomized
order. Methods run serially; parallelism only occurs within a method. Warmup and
measured rows are distinct. Rejected API sets are not repeatedly retried in every
block. Preflight is deterministic and always happens before warm measurements.

The report groups by session, environment/contract group, configuration,
verification class, and inventory digest. It displays median and interpolated
IQR. Speedup is the median of **paired per-block baseline/candidate ratios**,
not a ratio of unpaired medians. The 95% interval is a deterministic percentile
bootstrap with 2,000 resamples. At least three pairs are required; fewer than
15 receive a small-sample warning. This is a lightweight exploratory analysis,
not a universal significance guarantee or multiple-comparison correction.

A unique verified POSIX baseline is required in each paired block. Multiple
POSIX variants in a block make that baseline ambiguous and suppress the paired
interval. Cross-session or cross-digest timings are not pooled into paired
speedups. Keep raw rows; do not report only the fastest configuration chosen
from a large sweep. Tune on a separate corpus and confirm on evaluation corpora.

The supplied confirmation runner freezes bulk-par at two workers and 64 KiB,
with serial POSIX and POSIX-par at four workers as named baselines. It also
records two-worker POSIX and four-worker bulk as controls. Each of four new
read-only APFS corpora gets five separately seeded sessions of 30 measured
blocks, with exact preflight and one warmup per configuration:

```sh
make -j4 all
python3 tools/macos_benchmark_study.py ../confirmation-results
```

Results include raw rows, reports, per-session paired intervals, CPU/RSS and
process-wall measurements, fixture parameters, environment queries, source and
binary hashes, and image cleanup status. Corpus summaries give the median and
range of session estimates; they do not pool trials or invent a between-session
confidence interval. These sessions run consecutively on one host, so they do
not establish reproducibility across days or machines. `--quick` checks the
runner with smaller samples and is not a confirmation study.

## Controls and disclosure

`--diagnostic` adds per-batch clock calls and records API-call durations. Compare
diagnostic and non-diagnostic configurations separately. `readdir_calls` counts
userspace calls, not kernel syscalls; libc may buffer directory results. Requested
metadata bytes are not physical disk-read bytes. Physical I/O, power, thermal,
and Full Disk Access values are left unknown rather than guessed.

Use the same permissions and host application for candidates. Run whole-volume
catalog methods only with an explicit understanding of their source scope.
A subtree catalog experiment must report both `source_entries` and selected
`entries`; the time includes the whole-volume scan and filtering.

Put result files, inventories, and logs **outside** the corpus. The runner buffers
trial rows and creates its output file only after target scans finish. Shell
redirection can create a file earlier, so it remains the caller's responsibility
to keep redirected output away from the target. Output paths are never overwritten.
