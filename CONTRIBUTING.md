# Contributing

Changes should preserve equivalent scan results across backends and keep large
inventories within bounded resources. The project uses system libraries and has
no third-party runtime dependencies.

## Build and test

The C programs require a C17 compiler, Make, and POSIX threads. Python 3 is
required for integration tests and fixture tools. macOS provides the native bulk
and catalog APIs; Linux supports POSIX traversal and the portable test providers.

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

See [validation](docs/VALIDATION.md) for native APFS checks, opt-in performance
tests, and the platform coverage expected of a change.

## Code and tests

- Keep the existing C and Swift style. Prefer removing duplication and unused
  work to adding new layers or dependencies.
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
  Confirm equivalent results and distinguish CPU processing from filesystem I/O.

## Repository layout

| Path | Purpose |
| --- | --- |
| `src/` | Shared C scanner, benchmark CLI, inventory formats, and reporting |
| `cli/` | Terminal Vantage browser and binary export |
| `mac/` | SwiftUI app, Swift tests, app resources, and build scripts |
| `tests/` | C units/providers, Python integration tests, and captured fixtures |
| `tools/` | Corpus generation and native validation/benchmark runners |
| `docs/` | Usage, architecture, formats, benchmarking, and validation guides |
| `.github/` | CI and pull request guidance |

Build products, saved scans, benchmark output, and scratch files belong in the
ignored `build*/`, `output/`, and `tmp/` directories. Raw scans and logs can contain
personal paths; share a small synthetic reproduction when reporting a bug.
The source distribution includes the captured decoder fixtures and their
checksums, but excludes historical raw logs and local source snapshots.

## Submitting changes

Explain the concrete problem, resulting behavior, and validation in the pull
request. Include OS, compiler/Xcode version, scan method, and a synthetic fixture
for platform-specific issues. Preserve failed and unsupported benchmark results;
do not replace a backend to make a comparison pass.

Review `git status --short --untracked-files=all` and `git diff --cached --stat`
before publishing. Contributions are covered by the repository's [MIT license](LICENSE).
