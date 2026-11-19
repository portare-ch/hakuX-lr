#!/bin/sh
# Copyright (C) 2026-present PortareOS
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
deps=${HAKUX_DEPS:-$repo/build/deps}
build=${HAKUX_BUILD:-$repo/build/linux}
core_only=0
end_process=ON
if [ "${1:-}" = --core-only ]; then
    shift
    core_only=1
    end_process=OFF
    set -- -DHAKUX_BUILD_TESTS=OFF "$@"
fi
python3 tools/native/linux-deps.py --dest "$deps" --verify
cmake -S native -B "$build" -G Ninja \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAKUX_LTO=OFF \
    -DHAKUX_FEX_DIR="$deps/fex" -DNC_DEPS="$deps" \
    -DHAKUX_BUILD_LIBRETRO=ON -DHAKUX_VERSION="${HAKUX_VERSION:-dev}" \
    -DHAKUX_LR_END_PROCESS="$end_process" "$@"
if [ "$core_only" = 1 ]; then
    cmake --build "$build" --target hakux_libretro -j "${JOBS:-4}"
    exit
fi
cmake --build "$build" --target xboxrun hakux_libretro libretro-harness linux-smoke audio-smoke apu-stereo-smoke apu-resample-smoke apu-adpcm-test apu-reinit-smoke rumble-smoke voice-smoke usb-smoke nat-smoke lan-smoke xdvdfs-test vm-alias-test xc-test rtl-text-test contig-run-test dash-install-test \
    vulkan-probe kms-grab -j "${JOBS:-4}"
if grep -q '^HAKUX_DSP_JIT:BOOL=ON$' "$build/CMakeCache.txt"; then
    cmake --build "$build" --target apu-jit-smoke -j "${JOBS:-4}"
fi
if grep -q '^HAKUX_CPU:STRING=kvm$' "$build/CMakeCache.txt"; then
    cmake --build "$build" --target kvm-smoke kvm-rsqrt-test kvm-rsqrt-scan-test kvm-rsqrt-backend-test kvm-rsqrt-lane-test -j "${JOBS:-4}"
fi
ctest --test-dir "$build" --output-on-failure
