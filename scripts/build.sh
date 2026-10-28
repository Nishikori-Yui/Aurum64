#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
JOBS=${JOBS:-$(sysctl -n hw.logicalcpu)}
# Homebrew dylibs may require a newer OS than the app sources. The packager checks this.
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-26.0}
python3 scripts/prepare_core.py
CORE_VARIANT=$(printf '%s' "$(cat .build/sources/mupen64plus-core/.aurum-patch-stamp):$MACOSX_DEPLOYMENT_TARGET:OSD0:NOASM1:KEYBINDINGS0:O2" | shasum -a 256 | cut -c1-12)
mkdir -p .build/upstream
make -C .build/sources/mupen64plus-core/projects/unix -j"$JOBS" OSD=0 NO_ASM=1 KEYBINDINGS=0 OPTFLAGS=-O2 \
    TARGET_ARCH="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
    OBJDIR="$PWD/.build/upstream/core-$CORE_VARIANT" TARGET="$PWD/.build/upstream/core-$CORE_VARIANT.dylib" all
cp ".build/upstream/core-$CORE_VARIANT.dylib" .build/libmupen64plus.dylib
RSP_PIN=$(python3 scripts/source_inputs.py pin mupen64plus-rsp-cxd4)
RSP_VARIANT=$(printf '%s' "$RSP_PIN:$MACOSX_DEPLOYMENT_TARGET:O2" | shasum -a 256 | cut -c1-12)
make -C third_party/mupen64plus-rsp-cxd4/projects/unix -j"$JOBS" OPTFLAGS=-O2 \
    OBJDIR="$PWD/.build/upstream/rsp-$RSP_VARIANT" TARGET="$PWD/.build/upstream/rsp-$RSP_VARIANT.dylib" all
cp ".build/upstream/rsp-$RSP_VARIANT.dylib" .build/mupen64plus-rsp-cxd4.dylib
cmake -S . -B .build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"
cmake --build .build/native -j"$JOBS"
mkdir -p .build/app
swiftc -swift-version 6 -O -target "arm64-apple-macosx${MACOSX_DEPLOYMENT_TARGET}" \
    -import-objc-header Sources/AurumRuntime/include/AurumRuntime.h \
    -L .build/native -laurum-runtime -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
    Sources/AurumApp/*.swift -o .build/app/Aurum64
swiftc -swift-version 6 -parse-as-library -O -target "arm64-apple-macosx${MACOSX_DEPLOYMENT_TARGET}" \
    -import-objc-header Sources/AurumRuntime/include/AurumRuntime.h \
    -L .build/native -laurum-runtime -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
    Tests/SwiftStartupTests.swift -o .build/native/aurum-swift-startup-tests
python3 scripts/package.py
