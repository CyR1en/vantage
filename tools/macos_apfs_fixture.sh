#!/bin/bash
# Only a new private APFS image is created; original volumes are never modified.
set -euo pipefail
project="$(cd "$(dirname "$0")/.." && pwd)"
output="${1:-output/vantage-apfs-results}"
if [[ $# -gt 0 ]]; then shift; fi
make -C "$project" all
exec python3 "$project/tools/macos_validation.py" "$output" "$@"
