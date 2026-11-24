#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# The libretro core for RetroArch on Android (arm64-v8a): built with the NDK
# and packaged with its core info, stripped, with the licences and the
# revisions it was built from, as
# build/package/hakux-lr-<version>-android-arm64.zip.
#
#   ANDROID_NDK=<ndk> tools/native/android-libretro.sh <version>
#
# The dependencies are linux-deps.json's sources (tools/native/linux-deps.py),
# in build/deps or HAKUX_DEPS. Run on a clean checkout: the archive is the
# commit's. The DSP JIT is Linux's only, so Android's core has Fast and the
# DSP interpreter.
set -eu

version=${1:?usage: android-libretro.sh <version>}
ndk=${ANDROID_NDK:-${ANDROID_NDK_HOME:-}}
if [ -z "$ndk" ] || [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
    echo "android-libretro.sh: set ANDROID_NDK to the Android NDK (r29)" >&2
    exit 1
fi
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
deps=${HAKUX_DEPS:-$repo/build/deps}
build=${HAKUX_BUILD:-$repo/build/android-libretro}
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "android-libretro.sh: the checkout has changes; package a commit" >&2
    exit 1
fi
if [ ! -d "$deps/fex" ]; then
    echo "android-libretro.sh: no dependencies in $deps; run tools/native/linux-deps.py first" >&2
    exit 1
fi

cmake -S native -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=Release \
    -DNC_DEPS="$deps" -DHAKUX_FEX_DIR="$deps/fex" \
    -DHAKUX_BUILD_TESTS=OFF -DHAKUX_BUILD_LIBRETRO=ON -DHAKUX_DSP_JIT=OFF \
    -DHAKUX_VERSION="$version"
cmake --build "$build" --target hakux_libretro -j "${JOBS:-4}"

bin=$(echo "$ndk"/toolchains/llvm/prebuilt/*/bin)
out=$repo/build/package
name=hakux-lr-$version-android-arm64
rm -rf "${out:?}/$name" "$out/$name.zip"
mkdir -p "$out/$name/licenses"
"$bin/llvm-strip" -o "$out/$name/hakux-lr_libretro_android.so" "$build/hakux-lr_libretro_android.so"
cp LICENSE COPYING licenses/* "$out/$name/licenses/"
{
    echo "hakuX-lr $version for RetroArch on Android"
    echo "source $(git rev-parse HEAD) (https://github.com/portare-ch/hakuX-lr)"
    echo "compiler $("$bin/clang" --version | head -1) (NDK $(sed -n 's/^Pkg.Revision *= *//p' "$ndk/source.properties"))"
    echo "architecture arm64-v8a, Android 9 (API 28) or newer"
    echo "DSP JIT off"
    echo "runtime libc, libm, libdl, libz, liblog, libandroid, libaaudio; Vulkan through RetroArch's loader"
    echo "dependencies (tools/native/linux-deps.json):"
    cat tools/native/linux-deps.json
} > "$out/$name/BUILD-INFO"
sed "s/^display_version = .*/display_version = \"$version\"/" native/libretro/hakux-lr_libretro.info \
    > "$out/$name/hakux-lr_libretro.info"
python3 -I - "$out" "$name" "$(git log -1 --format=%ct)" <<'EOF'
import hashlib, os, sys, time, zipfile
out, name, stamp = sys.argv[1:]
when = time.gmtime(int(stamp))[:6]
path = os.path.join(out, name + '.zip')
with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
    root = os.path.join(out, name)
    for d, dirs, files in sorted(os.walk(root)):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(d, f)
            zi = zipfile.ZipInfo(os.path.join(name, os.path.relpath(full, root)), when)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, open(full, 'rb').read())
digest = hashlib.sha256(open(path, 'rb').read()).hexdigest()
with open(path + '.sha256', 'w') as f:
    f.write('%s  %s\n' % (digest, name + '.zip'))
print('%s  %s' % (digest, name + '.zip'))
EOF
