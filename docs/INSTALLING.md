# Installing the Vantage CLI

The installer downloads a prebuilt CLI from this repository's GitHub releases.
It supports macOS 11 or later and Linux on x86_64 and ARM64. Linux packages are
statically linked with musl, so they do not require a particular system glibc
version and can also run on musl distributions such as Alpine. The macOS CLI
has a lower minimum OS version than the graphical app.

## Install or update

Once the first CLI release has been published:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/latest/download/install.sh | sh
```

Rerun the command to update. It installs only `vantage` to `~/.local/bin`, without
sudo, a compiler, Xcode, Python, or a package manager. The installer needs `curl`,
`tar`, `gzip`, standard shell utilities, and either `sha256sum` or `shasum`.

If `~/.local/bin` is not already on your PATH, add this to `~/.zshrc` or
`~/.bashrc` and open a new terminal:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

Then run:

```sh
vantage --version
vantage ~/Documents
```

Choose a different destination:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/latest/download/install.sh |
  sh -s -- --install-dir "$HOME/bin"
```

Pin both the installer and the binary to a particular release:

```sh
curl --proto '=https' --tlsv1.2 -fsSL https://github.com/CyR1en/vantage/releases/download/v0.1.0/install.sh |
  sh -s -- --version v0.1.0
```

To inspect the script before running it, download it to a file with `curl -o
install.sh`, read it, and run `sh install.sh`. Remove an installation with
`rm "$HOME/.local/bin/vantage"`, or remove the executable from your chosen
destination. The installer does not edit shell configuration.

Linux uses POSIX scanning. `--allocated` and `--method bulk` require macOS.
Filesystem permissions still apply on both platforms.

## Verification and updates

The installer resolves `latest` to one version tag before fetching its archive
and checksum. It verifies SHA-256, reads only the executable from the archive,
and checks that the executable runs and reports the requested version. It then
replaces the destination using a rename on the same filesystem. Download,
checksum, and compatibility failures leave an existing executable intact.

All downloads use HTTPS. Checksums detect damaged or mismatched artifacts;
they are fetched from the same release and are not an independent publisher
signature. Installation trusts this repository and its release process.

## Is signing needed?

Linux does not require code signing to execute the CLI.

On Apple silicon, native executables need a valid code signature, which can be
an **ad hoc signature**. The release packager applies and verifies one on both
Mac architectures. This does not require an Apple Developer membership and
does not establish a verified publisher identity. See
[Apple's platform security documentation](https://support.apple.com/guide/security/secebb113be1/web).

For public Mac distribution that is trusted by Gatekeeper, use a **Developer ID
Application** certificate and Apple **notarization**. Ad hoc signing alone does
not provide that trust. Apple notes that tools such as `curl` do not quarantine
downloads, but Gatekeeper can also run in other circumstances; an ad hoc build
is not a guarantee that every Mac's policy will allow execution. See
[Apple's trusted execution guidance](https://developer.apple.com/forums/thread/706442)
and [distribution signing documentation](https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac).

The initial GitHub workflow produces ad hoc signed Mac binaries. It does not
import a Developer ID certificate or submit to Apple's notary service. The
packager accepts a locally available identity:

```sh
SIGN_IDENTITY='Developer ID Application: Your Name (TEAMID)' \
  sh tools/package-cli.sh v0.1.0
```

This enables a secure timestamp and hardened runtime, but signing alone is not
notarization. For a notarized release, integrate certificate import and notary
credentials into the Mac release jobs, submit a ZIP containing the final signed
executable with `xcrun notarytool submit ... --wait`, and require acceptance
before publishing. See [Apple's notarization workflow](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow).
Never put certificates, private keys, or passwords in the repository.

## Publishing CLI releases

The [CLI workflow](../.github/workflows/release-cli.yml) builds and tests four
packages:

| Archive suffix | Platform |
| --- | --- |
| `darwin-arm64` | Apple silicon, macOS 11+ |
| `darwin-x86_64` | Intel Mac, macOS 11+ |
| `linux-arm64` | 64-bit ARM Linux, static musl |
| `linux-x86_64` | Intel/AMD 64-bit Linux, static musl |

Each `vantage-vX.Y.Z-TARGET.tar.gz` contains `vantage` and `LICENSE`, with an
adjacent `.tar.gz.sha256` file. The release also includes `install.sh`.
Linux packages append the build system's musl copyright and license notices to
`LICENSE`. The packager reads `/usr/share/doc/musl/copyright` by default; set
`MUSL_LICENSE` to the matching notice file when using another musl toolchain.

1. Update `VANTAGE_VERSION` in `cli/vantage.h` for the release and commit the
   installer, packaging scripts, and workflow. Version `v0.1.0` matches the
   initial CLI version. The workflow rejects tags that disagree with it.
2. Optionally run `release-cli` manually to check all four builds. Manual runs
   upload workflow artifacts and do not create or publish a release.
3. Push a matching tag, for example `git tag v0.1.0` followed by
   `git push origin v0.1.0`.
4. The workflow runs the C and installer suites, builds packages, tests each
   packaged executable through the installer, and creates a **draft** release
   only after every platform passes.
5. Review the draft's notes and all nine assets (four archives, four checksums,
   and `install.sh`), then publish it. Tags with a hyphen, such as `v0.2.0-rc.1`,
   are marked as prereleases; users install them with `--version`. Stable
   releases appear through `latest`.

A retry may update a draft, but the workflow refuses to overwrite assets on an
already published release. Use a new version tag for changes to a public release.
Until a release is published, the public installation command has nothing to
download. Developing or testing these files locally does not publish them.
