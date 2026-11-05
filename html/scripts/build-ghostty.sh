#!/usr/bin/env bash
set -euo pipefail

# Build both WASM and JS from the same pinned Git source, never from npm artifacts.
revision=640db4617c69a73ccebce437cebc598c7f73cdb7
html_dir=$(cd "$(dirname "$0")/.." && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

command -v bun >/dev/null
command -v zig >/dev/null

git clone https://github.com/coder/ghostty-web.git "$work_dir/ghostty-web"
cd "$work_dir/ghostty-web"
git checkout --detach "$revision"
git apply "$html_dir/patches/ghostty-font-metrics.patch"
git apply "$html_dir/patches/ghostty-render-performance.patch"
git submodule update --init --recursive
bun install --frozen-lockfile
bun run build:wasm
bun run build:lib

vendor_dir="$html_dir/vendor/ghostty-web"
cp dist/ghostty-web.js dist/index.d.ts dist/__vite-browser-external-*.js ghostty-vt.wasm LICENSE "$vendor_dir/"
cp ghostty/LICENSE "$vendor_dir/GHOSTTY-LICENSE"
printf 'Built ghostty-web %s with Zig %s and Bun %s\n' "$revision" "$(zig version)" "$(bun --version)"
