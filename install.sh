#!/bin/sh
# Install a published Vantage CLI release on macOS or Linux.
set -eu

die() {
    printf 'vantage installer: %s\n' "$*" >&2
    exit 1
}

download() {
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --fail --silent --show-error --location --retry 3 \
        --connect-timeout 15 --max-time 180 "$@"
}

cleanup() {
    [ -z "$work" ] || rm -rf "$work"
    [ -z "$stage" ] || rm -rf "$stage"
}

main() {
    version=latest
    install_dir=
    repository=https://github.com/CyR1en/vantage
    work=
    stage=

    while [ "$#" -gt 0 ]; do
        case "$1" in
            --version|--install-dir)
                [ "$#" -ge 2 ] || die "$1 requires a value"
                [ -n "$2" ] || die "$1 requires a value"
                case "$1" in
                    --version) version=$2 ;;
                    --install-dir) install_dir=$2 ;;
                esac
                shift 2
                ;;
            --help|-h)
                cat <<'USAGE'
Usage: sh install.sh [--version vX.Y.Z] [--install-dir DIRECTORY]

Installs the latest published Vantage CLI to $HOME/.local/bin by default.
Supports macOS 11+ and Linux on x86_64 and ARM64. No sudo is used.
Rerun to upgrade, or select an exact release with --version.
USAGE
                return
                ;;
            *) die "unknown option: $1 (use --help)" ;;
        esac
    done

    if [ -z "$install_dir" ]; then
        [ -n "${HOME:-}" ] || die 'HOME is unset; use --install-dir'
        install_dir=$HOME/.local/bin
    fi
    case "$install_dir" in
        /*) ;;
        *) install_dir=$PWD/$install_dir ;;
    esac
    for tool in curl tar gzip mktemp; do
        command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
    done
    if command -v sha256sum >/dev/null 2>&1; then
        hash_tool=sha256sum
    elif command -v shasum >/dev/null 2>&1; then
        hash_tool=shasum
    else
        die 'SHA-256 verification requires sha256sum or shasum'
    fi

    os=$(uname -s)
    arch=$(uname -m)
    case "$os" in
        Darwin)
            os=darwin
            major=$(sw_vers -productVersion | cut -d. -f1)
            [ "$major" -ge 11 ] || die 'macOS 11 or later is required'
            if [ "$(sysctl -in sysctl.proc_translated 2>/dev/null || true)" = 1 ]; then
                arch=arm64
            fi
            ;;
        Linux) os=linux ;;
        *) die "unsupported operating system: $os" ;;
    esac
    case "$arch" in
        x86_64|amd64) arch=x86_64 ;;
        arm64|aarch64) arch=arm64 ;;
        *) die "unsupported architecture: $arch" ;;
    esac

    if [ "$version" = latest ]; then
        resolved=$(download --output /dev/null --write-out '%{url_effective}' \
            "$repository/releases/latest") || die 'could not find the latest release; check the connection and that a release is published'
        case "$resolved" in
            "$repository/releases/tag/"*) version=${resolved##*/} ;;
            *) die "unexpected release URL: $resolved" ;;
        esac
    fi
    case "$version" in
        v[0-9]*) ;;
        *) die 'version must be a release tag such as v0.1.0' ;;
    esac
    case "$version" in
        *[!a-zA-Z0-9._-]*) die 'invalid release tag' ;;
    esac

    asset=vantage-$version-$os-$arch.tar.gz
    url=$repository/releases/download/$version
    work=$(mktemp -d "${TMPDIR:-/tmp}/vantage-install.XXXXXX")
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM HUP
    printf 'Downloading Vantage %s for %s/%s...\n' "$version" "$os" "$arch"
    download --output "$work/$asset" "$url/$asset" || die "could not download $asset"
    download --output "$work/checksum" "$url/$asset.sha256" || die 'could not download the checksum'

    expected=$(awk -v name="$asset" '$2 == name { print $1 }' "$work/checksum")
    [ "${#expected}" -eq 64 ] || die 'invalid release checksum'
    case "$expected" in
        *[!0-9a-f]*) die 'invalid release checksum' ;;
    esac
    if [ "$hash_tool" = sha256sum ]; then
        actual=$(sha256sum "$work/$asset")
    else
        actual=$(shasum -a 256 "$work/$asset")
    fi
    actual=${actual%% *}
    [ "$actual" = "$expected" ] || die 'checksum mismatch; the existing installation was not changed'

    members=$(tar -tzf "$work/$asset") || die 'invalid release archive'
    [ "$members" = "$(printf 'vantage\nLICENSE')" ] || die 'unexpected release archive contents'
    # Read just the executable to stdout; never extract archive paths to disk.
    tar -xOzf "$work/$asset" vantage > "$work/vantage" || die 'could not read the release executable'
    [ -s "$work/vantage" ] || die 'release executable is empty'

    mkdir -p "$install_dir" || die "cannot create $install_dir; use --install-dir with a writable directory"
    [ ! -d "$install_dir/vantage" ] || die "$install_dir/vantage is a directory"
    stage=$(mktemp -d "$install_dir/.vantage-install.XXXXXX") || die "cannot write to $install_dir"
    cp "$work/vantage" "$stage/vantage"
    chmod 755 "$stage/vantage"
    installed_version=$("$stage/vantage" --version) || die 'the downloaded CLI cannot run on this system; the existing installation was not changed'
    [ "$installed_version" = "vantage ${version#v}" ] || die 'release version mismatch; the existing installation was not changed'
    mv -f "$stage/vantage" "$install_dir/vantage"
    printf 'Installed %s to %s/vantage\n' "$installed_version" "$install_dir"
    case ":${PATH:-}:" in
        *:"$install_dir":*) printf 'Run: vantage /path/to/folder\n' ;;
        *)
            printf 'Add this directory to your shell PATH: %s\n' "$install_dir"
            printf 'Or run the executable at: %s/vantage\n' "$install_dir"
            ;;
    esac
}

# Keep execution last so a truncated download cannot run a partial function.
main "$@"
