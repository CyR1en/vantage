# vantage

Find the directories and files contributing the most bytes under a directory.
The standalone C executable reuses scanbench's native scanner and keeps the
finished directory index in memory for browsing.

## Quick start

```sh
make -j4
./build/vantage ~/Documents
```

The browser opens automatically when input and output are terminals. It shows
the current directory's entries in descending size order, with bars indicating
their share of its known bytes. Enter opens the selected directory. The largest
files view searches the entire scanned tree, so a deeply nested large file is
easy to find without visiting every parent directory.

| Key | Action |
|---|---|
| Up / Down, `k` / `j` | Move selection |
| Enter / Right / `l` | Enter the selected directory |
| Left / Backspace / `h` | Go to the parent directory |
| `f` | Toggle the largest files across the entire scan |
| `g` | Return to the scanned root |
| `q` / Escape | Quit |

The view is a completed scan of a live filesystem, not a continuously updated
index. Run the command again after changing files. The terminal browser has no
file-deletion action; Vantage for Mac (below) adds Trash and delete actions.

## Ranked reports and JSON

When output is piped, vantage prints a plain report. Use `--no-interactive`
to get that report in a terminal as well:

```sh
./build/vantage ~/Documents --no-interactive --depth 2 --top 15
./build/vantage ~/Documents --files --top 30 --no-interactive
./build/vantage ~/Documents --json --depth 2 --top 20 > /tmp/vantage-results.json
```

Keep redirected output outside the scanned directory: the shell creates the
output file before the scanner starts. `--top` limits the number of entries at
each displayed parent and the number of largest files in plain/JSON output.
`--depth` limits displayed tree levels to 1..8. The interactive browser retains
all scanned entries regardless of these report limits.

JSON includes completion, the actual method and worker count, size contract,
path-attributed and unique byte totals, timing, ranked `items`, and, for a tree
report, `largest_files`. Each item includes its relative `path`, lossless
`path_hex`, kind, byte count, unknown-size count, display depth, and percentage.
Tree percentages are relative to the parent; global-file percentages are relative
to the scanned root. Unknown file sizes are JSON null. An error or invalid graph
produces an error object with empty result arrays and a nonzero exit status.

Terminal output escapes control characters and invalid pathname bytes. Use
`path_hex` for byte-exact programmatic pathname handling.

## Binary export for graphical front ends

Vantage retains the `SVX1` binary format and `.svx` extension for saved-scan compatibility.

`--export` writes the complete scanned tree to standard output as a compact
binary stream. While the scan runs, it writes `progress ENTRIES MILLISECONDS`
lines to standard error. It cannot be combined with `--json` or `--interactive`.
All integers are little-endian:

| Field | Type |
|---|---|
| Magic `SVX1` | 4 bytes |
| Completion (0 complete, 1 partial, 2 unsupported, 3 failed), size contract (0 logical, 1 allocated) | 2 × u32 |
| Scan ns, bytes, unique bytes, files, directories, unknown sizes, permission errors, other errors | 8 × u64 |
| Root path, reason, method | each u32 length + bytes |
| Entry count N | u64 |
| Per entry: parent index (N = scanned root), kind (1 file, 2 directory, 5 symlink), bytes (directory totals include descendants), unknown-size count, name | u64, u8, u64, u64, u32 length + bytes |

Names are raw pathname bytes. Nothing is written to standard output when the
scan fails; the reason goes to standard error, as it does for other modes.

## Vantage for Mac

`make app` builds `build/Vantage.app` using `mac/build-app.sh`: the SwiftUI
app (`mac/Sources/Vantage`) with the release `vantage` executable in
`Contents/Helpers`. Each scan runs the helper with `--export --timeout 0` and a
memory limit of half of physical memory, capped at 16 GiB. The scanner's resource
limits therefore can't terminate the app, and Cancel stops the helper process.

The standalone [Folio SVG](../mac/Support/Artwork/Folio.svg) is kept with the
app artwork. The layered icon lives in `mac/Support/AppIcon.icon`, editable in Icon Composer.
The build uses Xcode's `actool` to compile its vector layers and Liquid Glass
appearances into `Assets.car`, generate the `.icns` fallback, and merge the icon
metadata into the app's `Info.plist`.

The app's bundle identifier is `dev.scanbench.Vantage`. When upgrading from the
previous brand, configure Vantage's settings and grant Vantage Full Disk Access
for protected folders. Existing settings and cached scans remain under the
previous app identifier and are not migrated automatically.

- **Welcome:** choose a folder, pick a common location, or drop a folder on the
  window or the Dock icon.
- **Folders:** a squarified treemap (nested up to three levels, colored by
  kind) above a sortable list with share bars. Double-click or press Return to
  open a folder; ⌘↑ goes up; ⌘[ and ⌘] go back and forward; a path bar is shown
  at the bottom.
- **Largest Files and Kinds:** the 1,000 largest files across the scan,
  optionally filtered by kind (movies, pictures, archives, and so on).
- **Actions:** Space previews with Quick Look, ⇧⌘R shows the item in Finder, ⌥⌘C
  copies paths, ⌘⌫ moves items to the Trash, and ⌥⌘⌫ deletes them immediately.
  Every deletion is confirmed unless Settings turns off the Trash prompt. After a
  deletion, totals update without rescanning.
- **Saved scans and fast rescans:** each finished scan is saved in
  `~/Library/Caches/dev.scanbench.Vantage/Scans` with an FSEvents event ID
  taken just before the scan started. Reopening a folder (from Recent scans on the
  welcome screen, or its shortcut) shows the saved tree immediately, then updates
  it. **Rescan** (⌘R) replays FSEvents since that ID, re-lists only the folders
  that changed, and runs the helper only on new folders or folders whose events
  were coalesced. Unchanged folders are copied from the saved tree.
  **Full Rescan** (⌥⌘R) ignores the saved scan. If the event history is
  unavailable, was dropped, or belongs to a different volume, or if more than
  50,000 folders changed, the app falls back to a full scan.
- **Sizes:** the app measures actual disk usage by default (`--allocated`), so
  sparse virtual disks such as OrbStack's or Docker's `data.img` count only their
  allocated blocks, not their multi-hundred-gigabyte apparent length.
- **Settings:** switch to apparent file size, and
  open the Full Disk Access pane. Partial scans are flagged in the sidebar.

`make test-mac` builds the helper and runs model tests against it, including a
check that an incremental refresh matches a fresh full scan entry for entry.
Set `VANTAGE_PERF=/large/folder` to include the opt-in load-time test. See
[validation](VALIDATION.md#mac-app) for the available checks and
[benchmarking](BENCHMARKING.md) for measurement guidance.

## What the sizes mean

The default **logical** size is each regular file's data-fork length. Directory
sizes are sums of those file lengths beneath them. This matches the contract
used by the fastest benchmark comparisons.

`--allocated` (or `--size allocated`) requests Darwin's `ATTR_FILE_ALLOCSIZE`,
including the filesystem's reported allocation for file forks. This adds work
and can change the ranking of sparse or compressed files. It requires macOS.
Neither contract measures exclusive storage or bytes that deleting a file would
reclaim; shared APFS extents and snapshots make those different questions.

Hard-link names remain separate entries and contribute to each containing
directory. The summary's **unique file bytes** deduplicate by device and file ID.
Do not add nested directory totals together: parent totals already include their
descendants. Symlinks are not followed, other mounted filesystems are excluded,
and nonregular entries do not contribute regular-file bytes.

Missing permissions, disappearing entries, and unknown metadata can produce
**partial results**. The browser/report labels these totals as lower bounds,
and the command exits nonzero. A successful live scan is still a best-effort
view, not an atomic filesystem snapshot.

## Scanner choices and limits

The macOS default is `bulk-par`, two workers per available CPU capped at 16,
64 KiB buffers, and an 8,192-descriptor budget. Keeping queued directories open
avoids repeatedly reopening every ancestor in large trees. The helper raises
only its own descriptor soft limit, within the existing hard limit; the scanner
retains its checked, component-by-component fallback when descriptors are scarce.
Other platforms default to four POSIX workers and a 256-descriptor budget.
Select a worker count explicitly when you know your workload:

```sh
./build/vantage ~/Documents --workers 2
./build/vantage ~/Documents --method posix --workers 4
./build/vantage ~/Documents --method bulk --workers 4
```

`--method auto` uses bulk on macOS and POSIX on other platforms. If macOS bulk
is explicitly unsupported, auto reports the fallback and retries parallel POSIX.
It does not retry a partial scan as if that established completeness. Explicit
`--method bulk` reports unsupported instead of changing methods.
The report's Scan time covers the final scanner attempt; an automatic fallback
starts a fresh deadline.

The default scan deadline is 300 seconds, managed-allocation cap is 1 GiB, and
directory-descriptor budget is 8,192 on macOS and 256 elsewhere. Override with `--timeout`, `--memory-limit`,
and `--fd-limit`. `--timeout 0` disables the scan deadline; a filesystem syscall
can block beyond a deadline because checks happen between work items. The memory
limit covers tracked allocations, not total process RSS. All names and summaries
are retained, so memory grows with the number of entries.

## Install and test

The binary has no Python runtime requirement. Build with a C17 compiler and the
system thread library; native bulk/allocation features use macOS system APIs.

```sh
# Install to a user-owned prefix already on your PATH, or invoke it by full path:
make install-vantage PREFIX="$HOME/.local"
"$HOME/.local/bin/vantage" ~/Documents

# Release tests and sanitizer tests:
make -j4 test
make MODE=sanitize BUILD=build-sanitize -j4 test
```

Exit codes: 0 complete, 1 partial/failed, 2 unsupported, 64 usage, 71 fatal
allocation/system error. Ctrl-C exits 130. Terminal modes are restored when
leaving the browser.

See [validation](VALIDATION.md) for test coverage and native APFS checks.
