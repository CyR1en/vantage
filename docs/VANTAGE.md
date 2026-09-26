# Vantage

Vantage finds the largest directories and files under a folder. The terminal CLI
scans once, then opens a browser or prints a ranked report. Vantage for Mac adds
a treemap, saved scans, Quick Look, and file removal.

## Installation

The CLI installer supports macOS 11+ and Linux on x86_64 and ARM64. It installs
`vantage` to `~/.local/bin` without sudo. Linux binaries are statically linked
with musl. The graphical Mac app is built separately.

Once a CLI release is published, install or update with:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/latest/download/install.sh | sh
```

The installer needs `curl`, `tar`, `gzip`, and either `sha256sum` or `shasum`.
It verifies the download's SHA-256 and version before replacing an existing
installation. Checksums come from the same release, so installation trusts this
repository's release process. Mac CLI releases use ad hoc signing, without
Developer ID notarization.

If `~/.local/bin` is missing from your PATH, add this to `~/.zshrc` or `~/.bashrc`
and open a new terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

To choose another destination or pin a version:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/latest/download/install.sh |
	sh -s -- --install-dir "$HOME/bin"

curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/download/v0.1.0/install.sh |
	sh -s -- --version v0.1.0
```

Rerun the installer to update. To uninstall, remove `~/.local/bin/vantage` or the
executable in your chosen destination. The installer does not edit your shell
configuration. See [Contributing](../CONTRIBUTING.md) to build from source.

## Terminal browser

```sh
vantage ~/Documents
```

With no directory argument, Vantage scans the current directory. The browser
opens when input and output are terminals with cursor support. Entries appear
in descending size order, with bars showing their share of known parent bytes.

| Key | Action |
|---|---|
| Up / Down, `k` / `j` | Move selection |
| Enter / Right / `l` | Open the selected directory |
| Left / Backspace / `h` | Go to the parent directory |
| `f` | Toggle the largest files across the entire scan |
| `g` | Return to the scanned root |
| `q` / Escape | Quit |

The browser does not modify files. It shows a completed scan, so run the command
again after changing files.

## Ranked reports and JSON

Piped output is a plain report. `--no-interactive` prints that report in a terminal:

```sh
vantage ~/Documents --no-interactive --depth 2 --top 15
vantage ~/Documents --files --top 30 --no-interactive
vantage ~/Documents --json --depth 2 --top 20 > /tmp/vantage-results.json
```

Keep redirected output outside the scanned directory. The shell creates the
output file before scanning starts.

| Option | Effect |
|---|---|
| `--top N`, `-n N` | Rows per directory or largest-files report, 1–10,000; default 20 |
| `--depth N` | Displayed tree levels, 1–8; default 1 |
| `--files`, `-f` | Show the largest files across the scan |
| `--interactive` | Require an interactive terminal; incompatible with `--json` and `--export` |
| `--no-interactive` | Print the report and exit |
| `--json` | Print machine-readable rankings |

`--top` and `--depth` limit plain and JSON output. The browser retains all entries.

JSON includes completion, method, workers, size contract, total and unique bytes,
timing, and ranked `items`. Tree reports also include `largest_files`. Each item
has a relative `path`, lossless `path_hex`, kind, bytes, unknown-size count,
display depth, and percentage. Tree percentages use the parent total; global-file
percentages use the root total. Unknown file sizes are `null`. Errors or invalid
graphs produce an error object with empty result arrays and a nonzero exit code.

Terminal output escapes control characters and invalid pathname bytes. Use
`path_hex` for byte-exact pathname handling.

## Vantage for Mac

The app requires macOS 26+. Build it with `make app`, then open
`build/Vantage.app`. Building requires full Xcode 26+; see
[Contributing](../CONTRIBUTING.md).

Choose a folder, pick a common location, reopen a recent scan, or drop a folder
on the window or Dock icon. Grant Full Disk Access when prompted to scan
protected folders. **Vantage › Full Disk Access…** reopens the guide.

The left rail switches between folders, the 1,000 largest files, and file kinds.
The toolbar button or ⌃⌘S expands the rail into a resizable sidebar. In the
Folders view, drag the divider to resize the treemap or hide it from the location
bar. See the [design guide](DESIGN.md) for the interface conventions.

| Action | Shortcut |
|---|---|
| Open a selected folder | Return or double-click |
| Go up, back, or forward | ⌘↑, ⌘[, ⌘] |
| Search the current view | ⌘F |
| Show selection details | ⌥⌘I |
| Preview with Quick Look | Space |
| Show in Finder | ⇧⌘R |
| Copy paths | ⌥⌘C |
| Move to Trash | ⌘⌫ |
| Delete immediately | ⌥⌘⌫ |
| Rescan | ⌘R |
| Full Rescan | ⌥⌘R |

Moving to Trash asks for confirmation unless disabled in Settings. Immediate
deletion always asks for confirmation.
Totals update after deletion without rescanning. **Cancel** stops the scan.

Finished scans are cached in `~/Library/Caches/dev.cyr1en.Vantage/Scans`.
Reopening a folder displays its saved scan, then updates changed folders.
**Rescan** uses filesystem change history; **Full Rescan** starts over. If that
history is unavailable or too many folders changed, the app runs a full scan.

The app measures allocated disk usage by default. Settings can switch to
apparent file size. An orange dot in the rail's scan summary marks partial
results. The app uses the bundle identifier `dev.cyr1en.Vantage`. Settings,
cached scans, and Full Disk Access grants tied to an earlier bundle identifier
are not migrated; configure the app and grant access again after upgrading.
Existing `.svx` exports remain readable.

## What the sizes mean

The CLI defaults to **logical** size: each regular file's data-fork length.
Directory totals sum file sizes beneath them.

`--allocated`, `-a`, or `--size allocated` selects the filesystem's reported
allocation for file forks. It requires macOS and can change the ranking of
sparse or compressed files. Neither size contract measures exclusive storage
or bytes reclaimed by deletion, because APFS extents and snapshots can share
storage.

Hard-link names contribute separately to their containing directories.
**Unique file bytes** deduplicate by device and file ID. Parent directory totals
already include their descendants, so do not add nested totals together.
Symlinks are not followed, mounted filesystems are excluded, and nonregular
entries contribute no regular-file bytes.

Missing permissions, disappearing entries, or unknown metadata can produce
**partial results**. The CLI labels these totals as lower bounds and exits
nonzero. A live scan is a best-effort view, not an atomic filesystem snapshot.

## Scanner options and limits

| Option | Default and limits |
|---|---|
| `--size logical\|allocated` | `logical` in the CLI; `allocated` requires macOS |
| `--method auto\|bulk\|posix` | `auto`: bulk on macOS, POSIX elsewhere |
| `--workers N`, `-j N` | 1–64; default twice the CPU count capped at 16 on macOS, 4 elsewhere |
| `--timeout SECONDS` | 300; `0` disables the deadline |
| `--memory-limit BYTES` | 1 GiB; accepts `B`, `KiB`, `MiB`, and `GiB` suffixes |
| `--fd-limit N` | Directory-descriptor budget, minimum 4; default 8,192 on macOS, 256 elsewhere |
| `--help`, `-h` | Show all options |
| `--version` | Show the version |

`auto` retries POSIX only when bulk scanning is unsupported. It reports the
fallback and starts a new deadline. Explicit `--method bulk` reports unsupported
instead. Partial scans are not retried; scan time covers the final attempt.

Deadline checks happen between work items, so a filesystem call can block past
the deadline. The memory limit covers tracked allocations, not total process
memory. Vantage retains all names and summaries, so memory use grows with the
number of entries. The Mac app disables the deadline and sets the helper's
memory limit to half of physical memory, capped at 16 GiB.

Exit codes are 0 for complete, 1 for partial or failed, 2 for unsupported,
64 for usage errors, and 71 for fatal resource errors. Ctrl-C exits 130.
The browser restores terminal modes when it exits.

## Binary export for graphical front ends

`--export` writes the collected tree to standard output in `SVX1` format. Saved
scans retain the `.svx` extension. During scanning, standard error receives
`progress ENTRIES MILLISECONDS` lines. Export cannot be combined with `--json`
or `--interactive`.

All integers are unsigned and little-endian. Fields occur in this order:

| Header field | Type |
|---|---|
| Magic `SVX1` | 4 bytes |
| Completion: 0 complete, 1 partial, 2 unsupported, 3 failed | u32 |
| Size contract: 0 logical, 1 allocated | u32 |
| Scan nanoseconds, bytes, unique bytes, files, directories, unknown sizes, permission errors, other errors | 8 × u64 |
| Root path, reason, method | Each u32 byte length + bytes |
| Entry count N | u64 |

Exactly N entry records follow:

| Entry field | Type |
|---|---|
| Parent index; N denotes the scanned root | u64 |
| Kind: 0 unknown, 1 regular file, 2 directory, 3 block device, 4 character device, 5 symlink, 6 socket, 7 FIFO | u8 |
| Bytes | u64 |
| Unknown-size count | u64 |
| Name | u32 byte length + raw pathname bytes |

Entries use inventory order. Parent indexes are zero-based and may refer to
later entries. The scanned root has no entry record. Names are nonempty path
components with no NUL or `/` bytes. `.` and `..` are not valid names.

The CLI exports directory totals including descendants. An unknown file size
uses zero bytes and an unknown-size count of one. Other entry kinds have zero
bytes. Mac app saved scans can store zero for directory totals; the app
recomputes those totals from file entries when loading.

Partial scans can produce a stream and still exit nonzero. Failed or unsupported
scans, including invalid directory graphs, write no stream and report the reason
to standard error. Consumers must check the exit status and completion field.
