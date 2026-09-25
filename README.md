# Vantage

**Vantage** is a disk-usage browser for the terminal and macOS. **Vantage.app**
provides a native interface with saved scans and FSEvents-based refresh.

The shared C17 scanner also powers **scanbench**, a filesystem benchmark with
POSIX, bulk-metadata, and catalog-search backends. It compares algorithms for
equivalent, correct results. See [implementation coverage](docs/IMPLEMENTATION.md)
for supported methods and limits.

Native tests compare full manifests and canonical allocation against an
independent Python oracle. See [validation instructions](docs/VALIDATION.md)
for coverage and limitations.

## Build on a Mac

An installed Apple command-line toolchain is required. Python 3 is needed for tests and fixture generation, **not** to run the CLI.

```sh
make -j4
./build/scanbench --help
make test
```

The default macOS release build uses C17, `-O3`, and LTO, with an intended macOS 11+ deployment target. It links to system libraries only. Build metadata records compiler, flags, source fingerprint, and the selected SDK version. Make rebuilds when those settings change; separate `BUILD` directories keep configurations side by side.

```sh
make CC=clang MODE=debug BUILD=build-debug
make CC=clang MODE=sanitize BUILD=build-sanitize test
# Disable link-time optimization for a controlled build comparison:
make clean && make LTO=
```

On Linux, `posix` and `posix-par` work for development and tests. Darwin methods report `unsupported`; they are never relabeled POSIX fallbacks.

## Find large files with vantage

`make` also builds **vantage**, a standalone terminal disk-usage browser using
the same verified scanner. On macOS it defaults to bulk traversal with up to
16 workers, 64 KiB buffers, and an 8,192-descriptor budget; elsewhere it uses
four-worker parallel POSIX. Scanning happens once; browsing
and switching to the largest files use the retained results.

```sh
./build/vantage ~/Documents
./build/vantage ~/Documents --files --top 20 --no-interactive
./build/vantage ~/Documents --depth 2 --json
./build/vantage ~/Documents --allocated
```

Use arrows or `j`/`k` to move, Enter to enter a directory, Left or Backspace to go
up, `f` for the largest files across the scan, `g` for the root, and `q` to quit.
Piped output is a plain ranked report with size bars. Directory totals count
hard-link names separately; unique file bytes are shown alongside them. Logical
sizes are the default; `--allocated` requests filesystem allocation on macOS.

See [the vantage guide](docs/VANTAGE.md) for size semantics, options, JSON,
and installation. The benchmark CLI keeps its explicit unsupported behavior;
vantage's `--method auto` can retry parallel POSIX when bulk is unsupported.

## Vantage for Mac

`mac/` contains **Vantage.app**, a native SwiftUI front end for vantage.
It scans a folder or disk with the same helper, then shows an interactive
treemap and a ranked list you can drill into. You can Quick Look, reveal in
Finder, copy paths, move items to the Trash, or delete them immediately (after
confirming). It needs macOS 26 or later and Xcode (the Command Line Tools
alone lack SwiftUI's macros).

```sh
make app                 # builds build/Vantage.app
make test-mac            # builds the helper and runs Swift tests
open build/Vantage.app
open -a build/Vantage.app ~/Downloads   # scan a folder directly
```

See [the vantage guide](docs/VANTAGE.md#vantage-for-mac) for details.

## First comparison

Create a dedicated corpus **outside the project**, then compare subtree-capable methods. The generator refuses an existing destination.

```sh
python3 tools/fixture.py ../scanbench-corpus \
  --shape mixed --files 10000 --directories 1000

./build/scanbench verify ../scanbench-corpus \
  --methods posix,bulk,bulk-par --consistency quiescent

./build/scanbench bench ../scanbench-corpus \
  --methods posix,bulk,bulk-par --workers 1,2,4,8 \
  --buffer 64KiB,256KiB --rounds 15 --warmup 1 \
  --consistency quiescent --out ../scanbench-results.jsonl

./build/scanbench report ../scanbench-results.jsonl
```

Output files must be new; existing files are not overwritten. `quiescent` is your assertion that the corpus is not changing—it does not freeze the filesystem.

### Include filesystem-level catalog methods

`catalog` and `catalog-pipe` use `searchfs()`. It searches a **whole volume**, not just the supplied subtree. By default these methods reject a subtree root. Use an actual volume root, or explicitly opt into paying for a whole-volume scan plus subtree filtering:

```sh
./build/scanbench bench /Volumes/YourTestVolume \
  --methods posix,bulk,bulk-par,catalog,catalog-pipe \
  --workers 1,2,4,8 --buffer 256KiB \
  --rounds 15 --warmup 1 --out ../volume-results.jsonl

# Explicitly allow catalog's larger source scan, including all of its cost:
./build/scanbench bench ../scanbench-corpus \
  --methods posix,bulk,catalog,catalog-pipe --allow-volume-scan \
  --rounds 15 --out ../subtree-via-volume.jsonl
```

A driver may reject a required attribute set, use incompatible hard-link identities, or expose a different accessible namespace. Such a method is reported as unsupported, partial, or mismatching—not silently “fixed” into a different benchmark.

For a safer, controlled whole-volume experiment, the supplied script creates a **new private APFS disk image**, populates it, remounts it read-only, runs comparisons, then detaches and removes its own image. Results are retained in a new output directory. It does not use `sudo`, format an existing device, or purge system caches.

```sh
tools/macos_apfs_fixture.sh ../apfs-benchmark-results
```

The script validates all standard methods against an independent oracle, retains compressed manifests and mismatch details, then performs a **warm-cache** experiment. Source/binary hashes, commands, logs, and power/thermal query output are retained outside the image. Remounting does not establish a cold system. Use `--validate-only` to omit timings, or `--executable build-sanitize/scanbench` to validate a sanitizer build.

For confirmation across wide, deep, mixed, and larger corpora, run
`python3 tools/macos_benchmark_study.py ../confirmation-results`. Worker settings
are workload-dependent; use the [benchmark protocol](docs/BENCHMARKING.md)
to compare them on representative data.

## Methods

| Method | Implementation | Scope |
|---|---|---|
| `posix` | `readdir` + descriptor-relative `fstatat` | Subtree, one filesystem |
| `posix-par` | Same baseline with bounded worker pool | Subtree; additional control |
| `bulk` | `getattrlistbulk`, one worker | Subtree, macOS |
| `bulk-par` | Bulk enumeration with parallel directory traversal | Subtree, macOS |
| `catalog` | Volume-wide `searchfs`, parent-ID join, checked no-follow metadata enrichment | Volume; subtree requires opt-in |
| `catalog-pipe` | Catalog producer with overlapping parsing, then checked metadata enrichment | Same as catalog |
| `catalog-partition` | Disjoint 64-bit file-ID predicates in 1/2/4 independent searches | Experimental, opt-in |

`--methods all` includes the first six, not the speculative partitioned search. An existing catalog primitive is not itself proof of better performance.

## Experiments

```sh
# Nearby file-ID bands versus native DFS: run both commands on the same
# stable fixture; reports do not manufacture paired cross-session intervals.
./build/scanbench bench ../scanbench-corpus --methods posix,bulk-par \
  --workers 4 --order dfs --out ../dfs.jsonl
./build/scanbench bench ../scanbench-corpus --methods posix,bulk-par \
  --workers 4 --order id-band --frontier 32,256,2048 \
  --id-shift 8,12,16 --out ../id-bands.jsonl

# Other independent controls; each command still has exact preflight:
./build/scanbench bench ../scanbench-corpus --methods posix,bulk-par \
  --workers 4 --pack-invalid --out ../packing.jsonl
./build/scanbench bench ../scanbench-corpus --methods posix,bulk-par \
  --workers 8 --adaptive --out ../adaptive.jsonl
./build/scanbench bench ../scanbench-corpus --methods posix,bulk-par \
  --workers 4 --reduce hash --out ../hash-reduction.jsonl

# Omit object type: generic readdirattr path plus charged metadata enrichment.
./build/scanbench bench ../scanbench-corpus --methods posix,bulk \
  --omit-objtype --out ../omit-type.jsonl
```

Catalog enrichment validates parent components and object identities, corrects nonregular file types, and includes all required lookup work in scan time. The experimental partitioned search may be rejected by the filesystem; it remains explicitly unsupported on the tested APFS driver.

Further controls: `--order fifo|random`, `--extra entrycount,linkcount,uuid`, and `--skip-empty --consistency immutable`. Empty-directory elision must only be enabled for a genuinely immutable view. The flag is an assertion, not a snapshot or permission change.

File IDs are **not physical block addresses**. ID-band locality and catalog predicate partitioning are hypotheses being tested, not documented storage-layout guarantees.

## Contracts and interpretation

The default task is `tree`: a complete retained name/parent graph plus per-directory totals. `usage` omits the retained graph, but still computes counts, a digest, and unique-object totals. `enumerate` additionally omits requested byte lengths. These tasks are not ranked together.

Logical bytes mean regular-file **data-fork length**. Allocated bytes mean regular-file **`ATTR_FILE_ALLOCSIZE`**, retrieved with the Darwin API; the POSIX baseline pays for that additional lookup. It is not silently replaced with `st_blocks * 512`. Neither is advertised as bytes that deletion would reclaim.

Hard-link names remain separate entries; `bytes` is path-attributed, while `unique_bytes` deduplicates by device and 64-bit file ID. Symlinks are not followed. Hidden files and package contents are included. Other filesystem boundaries are excluded. Unknown attributes and inaccessible directories are not treated as zero-byte successes.

Native scans remain subject to permissions and macOS privacy controls. Full Disk Access status is not guessed, and root access is not presented as universal access. Start with a controlled fixture rather than `/` or a live cloud-synchronized home directory.

## Inventory exports and replay

```sh
./build/scanbench scan ../scanbench-corpus --method posix \
  --manifest ../inventory.sbi --out ../scan-result.jsonl
./build/scanbench replay ../inventory.sbi --rounds 15 --reduce hash \
  --out ../collector-replay.jsonl
```

Replay loads the normalized inventory before timing. It measures name copying, hashing, reduction, and graph construction in one process—not filesystem I/O or Darwin packed-record decoding. It has a separate event/method label and is excluded from scan leaderboards.

## Development

The shared C scanner is in `src/`, the terminal browser in `cli/`, and the native
app in `mac/`. Tests live beside each implementation; fixture and benchmark tools
are in `tools/`. Build products and local scan evidence are ignored.

See [CONTRIBUTING.md](CONTRIBUTING.md) for setup, checks, and contribution guidance.

## Read next

[Vantage guide](docs/VANTAGE.md) · [Benchmark protocol](docs/BENCHMARKING.md) · [Implementation coverage](docs/IMPLEMENTATION.md) · [Formats](docs/FORMAT.md) · [Validation](docs/VALIDATION.md) · [API sources](docs/SOURCES.md)

[MIT license](LICENSE). No telemetry, network requests, resident daemon, or bundled third-party runtime.
