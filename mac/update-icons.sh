#!/bin/sh
# Regenerates Support/Icons.xcassets from a checkout of github.com/phosphor-icons/core.
# Usage: mac/update-icons.sh PATH/TO/phosphor-icons/core
set -eu

here=$(cd "$(dirname "$0")" && pwd)
core=${1:?usage: $0 PATH/TO/phosphor-icons/core}
catalog="$here/Support/Icons.xcassets"
[ -d "$core/assets/regular" ] || { echo "$core does not look like phosphor-icons/core" >&2; exit 1; }

rm -rf "$catalog"
mkdir -p "$catalog"
printf '{\n  "info" : { "author" : "xcode", "version" : 1 }\n}\n' > "$catalog/Contents.json"

count=0
grep -v '^#' "$here/Support/Icons.txt" | grep -v '^[[:space:]]*$' | while read -r name; do
    for weight in regular bold fill duotone; do
        if [ "$weight" = regular ]; then file="$name.svg"; else file="$name-$weight.svg"; fi
        source="$core/assets/$weight/$file"
        [ -f "$source" ] || { echo "missing $weight/$file" >&2; exit 1; }
        set="$catalog/ph.$name.$weight.imageset"
        mkdir -p "$set"
        # A 24 pt intrinsic size (the 256-unit viewBox would otherwise be 256 pt).
        sed 's/<svg /<svg width="24" height="24" /' "$source" > "$set/$file"
        cat > "$set/Contents.json" <<JSON
{
  "images" : [ { "filename" : "$file", "idiom" : "universal" } ],
  "info" : { "author" : "xcode", "version" : 1 },
  "properties" : { "preserves-vector-representation" : true, "template-rendering-intent" : "template" }
}
JSON
    done
done
cp "$core/LICENSE" "$here/Support/Phosphor-LICENSE.txt"
echo "==> $(ls "$catalog" | grep -c imageset) images in $catalog"
