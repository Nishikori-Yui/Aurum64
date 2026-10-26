#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $(uname -m) == arm64 ]] || { echo 'Apple Silicon is required.' >&2; exit 1; }
xcrun --find metal >/dev/null
for tool in cmake ninja pkg-config python3; do
    command -v "$tool" >/dev/null || { echo "Install $tool (Homebrew is supported)." >&2; exit 1; }
done
pkg-config --exists sdl2 libpng || { echo 'Install sdl2 and libpng with Homebrew.' >&2; exit 1; }
git submodule update --init --recursive
echo 'Dependencies initialized. Run scripts/build.sh.'
