#!/bin/sh
# Source this file before invoking SwiftUI or app-bundle build tools.
if [ "$(uname -s)" != Darwin ]; then
    echo "Vantage.app requires macOS 26 or later and full Xcode." >&2
    exit 1
fi

# Respect an explicit toolchain; otherwise fall back from Command Line Tools
# to a standard Xcode installation without changing the system's selection.
if [ -z "${DEVELOPER_DIR:-}" ] && ! xcrun --find actool >/dev/null 2>&1; then
    for vantage_xcode in /Applications/Xcode.app /Applications/Xcode-beta.app; do
        if [ -d "$vantage_xcode/Contents/Developer" ]; then
            DEVELOPER_DIR="$vantage_xcode/Contents/Developer"
            export DEVELOPER_DIR
            break
        fi
    done
fi
if ! xcrun --find actool >/dev/null 2>&1 || ! xcrun --find swift-plugin-server >/dev/null 2>&1; then
    echo "Install full Xcode and select it with DEVELOPER_DIR to build or test Vantage.app." >&2
    exit 1
fi
