#!/bin/sh
# Builds Vantage.app: the SwiftUI front end plus the vantage scanner helper.
# Usage: mac/build-app.sh [OUTPUT_DIR]   (default: build/)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
out=${1:-"$repo/build"}
. "$here/toolchain.sh"
mkdir -p "$out"
out=$(cd "$out" && pwd)
app="$out/Vantage.app"

echo "==> Building scanner helper"
make -C "$repo" -j4 BUILD="$out" "$out/vantage" >/dev/null

echo "==> Building Vantage"
(cd "$here" && swift build -c release --product Vantage)
bin=$(cd "$here" && swift build -c release --show-bin-path)

echo "==> Compiling Folio app icon and Phosphor icons"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/resources"
deployment_target=$(plutil -extract LSMinimumSystemVersion raw -o - "$here/Support/Info.plist")
xcrun actool "$here/Support/AppIcon.icon" "$here/Support/Icons.xcassets" \
    --compile "$tmp/resources" \
    --app-icon AppIcon \
    --platform macosx \
    --minimum-deployment-target "$deployment_target" \
    --target-device mac \
    --output-partial-info-plist "$tmp/icon-info.plist" \
    --output-format human-readable-text --notices --warnings

echo "==> Assembling $app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Helpers" "$app/Contents/Resources"
cp "$here/Support/Info.plist" "$app/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Merge \"$tmp/icon-info.plist\"" "$app/Contents/Info.plist"
cp "$bin/Vantage" "$app/Contents/MacOS/Vantage"
cp "$out/vantage" "$app/Contents/Helpers/vantage"
ditto "$tmp/resources" "$app/Contents/Resources"
cp "$here/Support/Phosphor-LICENSE.txt" "$app/Contents/Resources/Phosphor-LICENSE.txt"
printf 'APPL????' > "$app/Contents/PkgInfo"

# Sign with a stable identity when one exists: privacy grants such as Full Disk
# Access are tied to the signature, and an ad-hoc signature changes every build.
# Override with SIGN_IDENTITY="Developer ID Application: …" or SIGN_IDENTITY=- (ad hoc).
identity=${SIGN_IDENTITY:-$(security find-identity -v -p codesigning 2>/dev/null |
    sed -n 's/.*"\(Apple Development: .*\)"$/\1/p' | head -1)}
identity=${identity:--}
[ "$identity" = "-" ] && echo "==> Signing ad hoc (Full Disk Access must be re-granted after each build)" \
                       || echo "==> Signing with $identity"
codesign --force --timestamp=none --sign "$identity" "$app/Contents/Helpers/vantage"
codesign --force --timestamp=none --sign "$identity" "$app"
echo "==> Done: $app"
