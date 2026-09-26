# Vantage output formats

The terminal CLI produces ranked JSON for scripts and a complete binary tree for
the Mac app. The shared scanner's C structs are internal, not a persistent ABI.

## Ranked JSON (schema 1)

`vantage DIRECTORY --json` writes one JSON object. Its fields include:

| Fields | Meaning |
|---|---|
| `schema` | Format version, currently 1 |
| `root`, `root_hex` | Display root and lossless hexadecimal pathname bytes |
| `method`, `workers`, `buffer_bytes` | Actual backend, worker count, and bulk buffer size |
| `size` | `logical` data-fork length or `allocated` file-fork allocation |
| `completion`, `reason`, `graph_valid` | Scan outcome and directory graph validity |
| `total_bytes`, `unique_bytes`, `unknown_sizes` | Bytes by pathname, bytes deduplicated by identity, and unknown-size file count |
| `scan_ns`, `index_ns` | Scan and view-index durations in nanoseconds |
| `entries`, `files`, `directories` | Counts excluding the scanned root |
| `view` | `tree` or `files` |
| `items` | Ranked rows limited by `--top` and `--depth` |
| `largest_files` | Global file rankings, included in the tree view |

Rows contain `path`, `path_hex`, `kind`, `bytes`, `unknown_sizes`, `depth`, and
`percent`. `path` is relative to the scanned root. Use `path_hex` when exact
pathname bytes matter. An unknown file size is JSON `null`, never an inferred
zero. Directory bytes include descendants; do not add nested totals together.
Tree percentages use the parent total; global file percentages use the root.

`completion` is `complete`, `partial`, `unsupported`, or `failed`. Partial totals
are lower bounds. A failed or unsupported scan produces an error object with
`schema`, root fields, `completion`, `reason`, and an empty `items` array; the
success-only fields above may be absent. Always check the process exit status.

The method label is `posix` or `bulk`; concurrency is represented separately by
`workers`. Consumers should treat the method as descriptive metadata. Byte
counts and durations are JSON integers, so consumers must preserve 64-bit integer
precision. Output must be redirected outside the directory being scanned.

## Complete binary tree (SVX1)

`vantage DIRECTORY --export` writes the full tree to standard output, with
progress on standard error. The Mac app reads this stream and stores saved scans
with the `.svx` extension. The wire format and existing saved files remain
compatible.

See [the SVX1 specification](VANTAGE.md#binary-export-for-graphical-front-ends)
for field order, little-endian encoding, parent indexes, unknown sizes, and
completion handling. Exported entries are not limited by `--top` or `--depth`.
Partial streams still require checking the nonzero process exit status; failed
or unsupported scans do not produce a stream.
