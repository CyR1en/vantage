# Vantage

A disk-usage browser for macOS and the terminal. Find large files, explore folders,
and use the Mac app's treemap, saved scans, and incremental rescans.

## Mac app

Requires macOS 26+ and Xcode 26+.

```sh
make app
open build/Vantage.app
```

## Terminal

Install the CLI on macOS 11+ or Linux (Intel/AMD 64-bit or ARM64):

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/latest/download/install.sh | sh
vantage ~/Documents
```

The installer uses published GitHub releases, checks SHA-256, and installs to
`~/.local/bin` without sudo. Add that directory to your `PATH` if needed.
The download becomes available when the first CLI release is published.
See [installation and releases](docs/INSTALLING.md) for version pinning,
custom locations, and macOS signing.

To build from source, use a C17 compiler and Make:

```sh
make -j4
./build/vantage ~/Documents

# Show the 20 largest files
./build/vantage ~/Documents --files --top 20 --no-interactive
```

Use the arrow keys to navigate, Enter to open a folder, and `q` to quit.
See the [Vantage guide](docs/VANTAGE.md) for all controls, options, and installation.

## Development

Python 3 is required for the C test suite.

```sh
make test
make test-mac    # Mac app tests; requires Xcode
```

The repository also includes `scanbench`, a CLI for comparing filesystem-scanning
methods. Build it with `make` and run `./build/scanbench --help` to get started.

- [Contributing](CONTRIBUTING.md) and [validation](docs/VALIDATION.md)
- [Benchmarking](docs/BENCHMARKING.md) and [scanner architecture](docs/IMPLEMENTATION.md)
- [File formats](docs/FORMAT.md) and [API references](docs/SOURCES.md)

[MIT license](LICENSE).
