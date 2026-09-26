# Contributing

Changes should preserve Vantage's scan results and keep large inventories within
bounded resources. The CLI and Mac app share one scanner, using system libraries
with no third-party runtime dependencies.

## Build and test

The C helper requires a C17 compiler, Make, and POSIX threads. Python 3 is
required for integration tests and fixture tools. macOS provides the native bulk
metadata API; Linux supports POSIX traversal and the portable bulk test provider.

```sh
make -j4 test
make -j4 MODE=sanitize BUILD=build-sanitize test
```

Vantage.app requires macOS 26 or later and full Xcode 26 or later. From the
repository root:

```sh
make -j4 test-mac
SIGN_IDENTITY=- make -j4 app
codesign --verify --deep --strict build/Vantage.app
```

The Mac targets respect `DEVELOPER_DIR`. If Command Line Tools are selected,
they look for `/Applications/Xcode.app` and `/Applications/Xcode-beta.app`.
Set `DEVELOPER_DIR` explicitly for an installation elsewhere. Ad hoc signing
with `SIGN_IDENTITY=-` needs no developer certificate. It does not produce a
notarized release; Full Disk Access may need to be granted again after rebuilding.

`BUILD` selects the C/app output directory; `MODE` selects `release`, `debug`, or
`sanitize` for C. The Swift app and `test-mac` use release configuration.
`make clean` removes the standard C/app build directories, leaving Swift's
package cache and local evidence alone. Run `swift package --package-path mac clean`
to clear Swift build products if needed.

Linux checks POSIX traversal and simulated Darwin providers. Validate native
bulk metadata and allocated sizes on macOS. For changes to native scan behavior,
run the APFS checks with a new output directory:

```sh
python3 tools/macos_validation.py output/native-validation \
  --executable build/vantage --files 2000 --directories 200
```

The tool creates a test image, mounts it read-only for scanning, and detaches it
afterward. The app's optional performance tests accept a corpus through
`VANTAGE_PERF=/path/to/test-corpus make test-mac` or a saved scan through
`VANTAGE_EXPORT=/path/to/saved.svx make test-mac`.

## Code and tests

- Follow [the C style guide](docs/C_STYLE.md) and run `make format` before
  submitting C changes; `make format-check` verifies the result without editing
  files. Both commands require clang-format 22.1.0. Keep the existing Swift
  style. Prefer removing duplication and unused work to adding new layers or
  dependencies.
- Follow [the Mac app design guide](docs/DESIGN.md) for layout, components,
  icons, and interaction when changing or adding UI in `mac/`. Interface icons
  are vendored Phosphor SVGs; update them with `mac/update-icons.sh`.
- Keep scanner semantics explicit: no symlink traversal, one-device scope,
  hard-link identity, and unknown metadata must remain distinguishable.
- Persistent formats are versioned. Preserve compatibility or document a
  version change in [the format guide](docs/FORMAT.md) or
  [the Vantage export guide](docs/VANTAGE.md#binary-export-for-graphical-front-ends).
- Run the existing checks relevant to the change. Add regression tests for core
  accounting, decoding, concurrency, cancellation, cache consistency, and file
  operations when their behavior changes. Cosmetic edits, documentation, and
  straightforward cleanup do not need new tests.
- Measure performance changes against the same workload and build settings.
  Verify equal scan results before comparing timings. Record the workload and
  cache state, and distinguish CPU processing from filesystem I/O.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/` | Shared C scanner, accounting, and metadata decoder |
| `cli/` | Terminal Vantage browser and binary export |
| `mac/` | SwiftUI app, Swift tests, app resources, and build scripts |
| `tests/` | C units/providers, Python integration tests, and captured fixtures |
| `tools/` | Fixture generation, native correctness validation, formatting, and packaging |
| `docs/` | Design, file formats, and the Vantage usage guide |
| `.github/` | CI and pull request guidance |

Build products, saved scans, validation output, and scratch files belong in the
ignored `build*/`, `output/`, and `tmp/` directories. Keep research, development
notes, and historical documentation in the ignored `.local_docs/` directory.
Raw scans and logs can contain personal paths; share a small synthetic
reproduction when reporting a bug.
The source distribution includes the captured decoder fixtures and their
checksums.

## Submitting changes

Explain the concrete problem, resulting behavior, and validation in the pull
request. Include OS, compiler/Xcode version, scan method, and a synthetic fixture
for platform-specific issues. Keep partial, failed, and unsupported scans explicit;
only automatic method selection may retry an unsupported bulk scan with POSIX.

Review `git status --short --untracked-files=all` and `git diff --cached --stat`
before publishing. Contributions are covered by the repository's [MIT license](LICENSE).
