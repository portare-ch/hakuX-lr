#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Compatibility for saved installer links.
set -eu

script="$(dirname "$0")/setup-hakux-lr.sh"
[ ! -f "$script" ] || exec sh "$script" "$@"

url=https://raw.githubusercontent.com/portare-ch/hakuX-lr/master/tools/linux/setup-hakux-lr.sh
script=$(mktemp)
trap 'rm -f "$script"' EXIT
if command -v curl >/dev/null 2>&1; then
    curl -fsSL "$url" -o "$script"
else
    wget -qO "$script" "$url"
fi
sh "$script" "$@"
