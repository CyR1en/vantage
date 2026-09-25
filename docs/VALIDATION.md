# Validation

Run checks from the repository root. Python 3 is needed for the C integration
suite; no Python packages are required.

## C scanner and terminal browser

```sh
make -j4 test
make -j4 MODE=sanitize BUILD=build-sanitize test
```

The suites exercise collector accounting, graph construction, hard links,
captured Darwin records and malformed inputs, parallel scheduling, cancellation,
resource limits, inventory replay, benchmark statistics, terminal navigation,
and output encoding. Python compares native scan results with an independent
filesystem oracle. Synthetic providers exercise Darwin paths on Linux too.

Native bulk and allocated-size cases run on macOS. Linux checks POSIX methods
and explicitly unsupported Darwin methods. Invalid UTF-8 filename cases are
skipped on filesystems that reject those names; byte-oriented unit coverage
still runs.

## Mac app

macOS 26 or later and full Xcode 26 or later are required.

```sh
make -j4 test-mac
SIGN_IDENTITY=- make -j4 app
codesign --verify --deep --strict build/Vantage.app
```

`test-mac` builds the C helper and passes its path to Swift tests. Tests cover
tree decoding and serialization, refresh equivalence, immutable snapshots,
cache publication, stale deletion requests, navigation remapping, treemap
layout, and helper cancellation. File operations use temporary fixtures.

For Swift AddressSanitizer checks, use the same Xcode selection as the build:

```sh
. mac/toolchain.sh
VANTAGE_HELPER="$PWD/build/vantage" swift test --package-path mac \
  -c release --scratch-path build-swift-sanitize --sanitize address
```

The performance tests are opt-in because they scan user-selected data and may
wait for filesystem events:

```sh
VANTAGE_PERF=/path/to/test-corpus make test-mac
VANTAGE_EXPORT=/path/to/saved.svx make test-mac
```

## Native volume validation

The native harness creates its own APFS image, populates it, remounts it
read-only, compares scanner contracts, and detaches it afterwards:

```sh
python3 tools/macos_validation.py output/native-validation \
  --executable build/scanbench --validate-only --files 2000 --directories 200
```

The destination must be new. Use [the benchmark protocol](BENCHMARKING.md)
before collecting performance results. A passing mock suite does not establish
APFS driver behavior, privacy permissions, or equivalent real-world coverage.

## CI and evidence

[GitHub Actions](../.github/workflows/ci.yml) runs C release and sanitizer checks
on Linux and the configured macOS versions. A macOS 26 job runs Swift tests and
builds and verifies the app. Scheduled/manual jobs exercise a private read-only
APFS image. CI uploads logs as artifacts; configured jobs are not evidence that
they have run on a particular revision.

Raw local logs, benchmark inventories, and source snapshots are excluded from
the source distribution. Captured decoder batches
remain in [tests/fixtures/attrs](../tests/fixtures/attrs/README.md), alongside
their provenance and checksums.
