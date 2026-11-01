#!/bin/sh
# The x86-64 Linux build and its checks (#56), in the hakux-linux image
# built for amd64, as CI runs them (.github/workflows/linux.yml): the core
# with the KVM backend (HAKUX_CPU=kvm) and its tests (linux-build.sh), and
# the pure HLE tests. No package or release: those are aarch64's
# (linux-ci.sh).
#   sh tools/native/linux-ci-x86_64.sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
export HAKUX_DEPS="${HAKUX_DEPS:-$repo/build/deps}"
export HAKUX_BUILD="${HAKUX_BUILD:-$repo/build/linux-x86_64}"
jobs=${JOBS:-4}

python3 tools/native/linux-deps.py --dest "$HAKUX_DEPS"
sh tools/native/linux-build.sh

tests=$repo/build/hle-tests-x86_64
cmake -S tests/hle -B "$tests" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" \
    -DNC_DEPS="$HAKUX_DEPS" > /dev/null
cmake --build "$tests" -j "$jobs"
ctest --test-dir "$tests" --output-on-failure
echo "PASS: linux-ci-x86_64"
