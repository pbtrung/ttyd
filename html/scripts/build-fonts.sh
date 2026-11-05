#!/usr/bin/env bash
set -euo pipefail

# Latest official release, verified 2026-11-04. Pin and verify for repeatable builds.
version=2.304
checksum=6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf
html_dir=$(cd "$(dirname "$0")/.." && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

curl -fL "https://github.com/JetBrains/JetBrainsMono/releases/download/v$version/JetBrainsMono-$version.zip" -o "$work_dir/font.zip"
printf '%s  %s\n' "$checksum" "$work_dir/font.zip" | shasum -a 256 -c -
for style in Regular Bold Italic BoldItalic; do
    unzip -p "$work_dir/font.zip" "fonts/webfonts/JetBrainsMono-$style.woff2" > "$html_dir/src/fonts/JetBrainsMono-$style.woff2"
done
unzip -p "$work_dir/font.zip" OFL.txt > "$html_dir/src/fonts/OFL.txt"
