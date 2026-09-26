#!/bin/sh
# Package an already tested native release build.
set -eu

tag=${1:?Usage: package-cli.sh vX.Y.Z [BUILD_DIR] [OUTPUT_DIR]}
build=${2:-build}
output=${3:-output/cli-release}
case "$tag" in
    v[0-9]*) ;;
    *) printf 'Expected a version tag such as v0.1.0\n' >&2; exit 1 ;;
esac
case "$tag" in
    *[!a-zA-Z0-9._-]*) printf 'Invalid version tag\n' >&2; exit 1 ;;
esac
[ "$("$build/vantage" --version)" = "vantage ${tag#v}" ] || {
    printf 'Release tag does not match the CLI version in cli/vantage.h\n' >&2
    exit 1
}
case "$(uname -s)" in
    Darwin) os=darwin ;;
    Linux) os=linux ;;
    *) printf 'Unsupported release platform\n' >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    arm64|aarch64) arch=arm64 ;;
    *) printf 'Unsupported release architecture\n' >&2; exit 1 ;;
esac

mkdir -p "$output"
stage=$(mktemp -d "$output/.package.XXXXXX")
trap 'rm -rf "$stage"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
install -m 755 "$build/vantage" "$stage/vantage"
install -m 644 LICENSE "$stage/LICENSE"
if [ "$os" = linux ]; then
    readelf -l "$stage/vantage" > "$stage/elf-headers"
    if grep -q INTERP "$stage/elf-headers"; then
        printf 'Linux releases must be statically linked with musl\n' >&2
        exit 1
    fi
    printf '\n\nStatic Linux runtime (musl):\n\n' >> "$stage/LICENSE"
    cat "${MUSL_LICENSE:-/usr/share/doc/musl/copyright}" >> "$stage/LICENSE"
fi
if [ "$os" = darwin ]; then
    identity=${SIGN_IDENTITY:--}
    if [ "$identity" = - ]; then
        codesign --force --sign - --timestamp=none "$stage/vantage"
    else
        codesign --force --sign "$identity" --timestamp --options runtime \
            --identifier dev.scanbench.vantage.cli "$stage/vantage"
    fi
    codesign --verify --strict "$stage/vantage"
fi
asset=vantage-$tag-$os-$arch.tar.gz
COPYFILE_DISABLE=1 tar -czf "$output/$asset" -C "$stage" vantage LICENSE
(
    cd "$output"
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$asset" > "$asset.sha256"
    else
        shasum -a 256 "$asset" > "$asset.sha256"
    fi
)
printf '%s/%s\n' "$output" "$asset"
