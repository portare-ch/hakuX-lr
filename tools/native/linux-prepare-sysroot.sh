#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# Keep library symlinks inside the sysroot and use the host's newer KVM headers.
set -eu

sysroot=$(realpath -e "${1:?usage: linux-prepare-sysroot.sh <sysroot>}")
if [ "$sysroot" = / ] || [ ! -d "$sysroot/usr/include" ]; then
    echo "linux-prepare-sysroot.sh: invalid sysroot: $sysroot" >&2
    exit 1
fi

find "$sysroot" -type l -lname '/*' | while IFS= read -r link; do
    target="$sysroot$(readlink "$link")"
    relative=$(realpath -m -s --relative-to="$(dirname "$link")" "$target")
    ln -sfn "$relative" "$link"
done

multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
for directory in linux asm-generic "$multiarch/asm"; do
    rm -rf "$sysroot/usr/include/$directory"
    cp -RL "/usr/include/$directory" "$sysroot/usr/include/$directory"
done
