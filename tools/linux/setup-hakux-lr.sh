#!/bin/sh
# Copyright (C) 2026-present PortareOS
# SPDX-License-Identifier: GPL-2.0-or-later
# Sets up RetroArch with hakuX-lr for testing on an ARM64 or AMD64 Linux
# machine, then writes start-hakux-lr.sh, which starts games with it.
#
#   sh setup-hakux-lr.sh [options]
#
#   --version <v>     a release such as 0.17.1 or 0.18.0-exp.1
#                     (default: the newest stable release)
#   --tarball <file>  install a local release archive (linux-package.sh, CI)
#   --core <file.so>  install a locally built hakux-lr_libretro.so; the .info
#                     next to it is used when present
#   --games <dir>     where the games are (default: ~/roms/xbox)
#   --prefix <dir>    where everything goes (default: ~/.local/share/hakux-lr)
#   --no-packages     don't install distribution packages
#   --retroarch <k>   system or appimage (AppImage is AMD64 only).
#                     Defaults to RetroArch in PATH, then the distribution's
#                     package. On SteamOS or without a package manager,
#                     RetroArch's AppImage.
#   --no-steam        on SteamOS, don't add hakuX-lr to the Steam library
#   --no-auto-update  on SteamOS, disable automatic core updates
#
# It installs RetroArch, Vulkan and menu assets with the distribution's
# package manager (pacman, apt, dnf or zypper; asks for the sudo password),
# downloads the core and controller profiles, and gives hakuX-lr its own
# RetroArch configuration in the prefix, so an existing RetroArch setup is
# left alone. On AMD64 it checks KVM (/dev/kvm, TSC scaling, UMIP). Run it
# again to update the setup. Your own settings go in <prefix>/user.cfg.
#
# On SteamOS, installs RetroArch's AppImage in the prefix and adds hakuX-lr to Steam.
# Uses EmuDeck's roms/xbox folder when present. Setup does not request root;
# missing KVM access needs a separate fix before games can run.
#
# start-hakux-lr.sh runs on a desktop (Wayland, X11) or, without a display
# server, straight on KMS (Vulkan VK_KHR_display, udev input), as on
# PortareOS. To remove everything, delete the prefix and
# ~/.local/bin/start-hakux-lr.sh.
set -eu

REPO=portare-ch/hakuX-lr
# Pin the AMD64 AppImage archive; upstream provides no checksum file.
APPIMAGE_VERSION=1.22.2
APPIMAGE_URL=https://buildbot.libretro.com/stable/$APPIMAGE_VERSION/linux/x86_64/RetroArch.7z
APPIMAGE_SHA256=7d62da9a21397d6e1b9490785cedbeafd262781b50115076736fbe8a77ef30e9
AUTOCONFIG_URL=https://codeload.github.com/libretro/retroarch-joypad-autoconfig/tar.gz/refs/heads/master

say() { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() {
    if [ -f "$0" ]; then sed -n '3,/^set -eu$/p' "$0" | sed '$d; s/^# \{0,1\}//'
    else echo "see the comment at the top of setup-hakux-lr.sh"; fi
}

version=
tarball=
corefile=
games=
rakind=
steam=1
auto_update=1
prefix="${XDG_DATA_HOME:-$HOME/.local/share}/hakux-lr"
packages=1
while [ $# -gt 0 ]; do
    case $1 in
        --version) [ $# -ge 2 ] || die "$1 needs a value"; version=${2#linux-v}; version=${version#v}; shift ;;
        --tarball) [ $# -ge 2 ] || die "$1 needs a value"; tarball=$2; shift ;;
        --core) [ $# -ge 2 ] || die "$1 needs a value"; corefile=$2; shift ;;
        --games) [ $# -ge 2 ] || die "$1 needs a value"; games=$2; shift ;;
        --prefix) [ $# -ge 2 ] || die "$1 needs a value"; prefix=$2; shift ;;
        --no-packages) packages=0 ;;
        --retroarch) [ $# -ge 2 ] || die "$1 needs a value"; rakind=$2; shift ;;
        --no-steam) steam=0 ;;
        --no-auto-update) auto_update=0 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
    shift
done
[ -z "$tarball" ] || [ -f "$tarball" ] || die "no such file: $tarball"
[ -z "$corefile" ] || [ -f "$corefile" ] || die "no such file: $corefile"
case $rakind in
    ''|system|appimage) ;;
    flatpak) die "RetroArch's Flatpak is no longer supported: use --retroarch appimage (AMD64) or system" ;;
    *) die "--retroarch takes system or appimage" ;;
esac
os_id=$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"' | head -n1)
if [ -z "$games" ]; then
    # Prefer EmuDeck's folder on the internal drive or an SD card.
    games="$HOME/roms/xbox"
    for d in "$HOME/Emulation/roms/xbox" /run/media/*/Emulation/roms/xbox /run/media/*/*/Emulation/roms/xbox; do
        [ -d "$d" ] && { games=$d; break; }
    done
fi

case $(uname -m) in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) die "hakuX-lr runs on ARM64 and AMD64 Linux, not $(uname -m)" ;;
esac
[ "$(uname -s)" = Linux ] || die "hakuX-lr needs Linux"

if [ "$os_id" = steamos ]; then
    asroot=none
elif [ "$(id -u)" -eq 0 ]; then
    asroot=
elif have sudo; then
    asroot=sudo
elif have doas; then
    asroot=doas
else
    asroot=none
fi
root() {
    [ "$asroot" != none ] || die "run as root, or install sudo, for: $*"
    say "running as root: $*"
    $asroot "$@"
}

fetch() { # <url> <file>
    if have curl; then
        curl -fL --retry 3 --progress-bar -o "$2" "$1"
    elif have wget; then
        wget -q --show-progress -O "$2" "$1"
    else
        die "curl or wget is needed"
    fi
}
fetch_text() { # <url>
    if have curl; then curl -fsSL --retry 3 "$1"
    elif have wget; then wget -qO- "$1"
    else die "curl or wget is needed"; fi
}
sha256() {
    if have sha256sum; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# ---------------------------------------------------------------- packages

vulkan_icds() {
    for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d /usr/local/share/vulkan/icd.d; do
        [ -d "$d" ] && ls "$d"/*.json 2>/dev/null
    done | grep -v -e lvp_icd -e lavapipe || true
}
libvulkan() {
    for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/local/lib; do
        [ -e "$d/libvulkan.so.1" ] && { echo "$d/libvulkan.so.1"; return 0; }
    done
    return 1
}
have_libvulkan() { libvulkan >/dev/null; }
gpu_vendors() {
    cat /sys/class/drm/card[0-9]*/device/vendor 2>/dev/null | sort -u
}

install_packages() {
    want_ra=0 want_vk=0 want_loader=0
    have retroarch || want_ra=1
    [ -n "$(vulkan_icds)" ] || want_vk=1
    have_libvulkan || want_loader=1
    if [ $want_ra = 0 ] && [ $want_vk = 0 ] && [ $want_loader = 0 ]; then
        return 0
    fi
    vendors=$(gpu_vendors)
    if have pacman; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch retroarch-assets-ozone
        [ $want_loader = 0 ] || set -- "$@" vulkan-icd-loader
        if [ $want_vk = 1 ]; then
            case $vendors in *0x1002*) set -- "$@" vulkan-radeon ;; esac
            case $vendors in *0x8086*) set -- "$@" vulkan-intel ;; esac
        fi
        [ $# -eq 0 ] || root pacman -S --needed --noconfirm "$@" || die "installing $* failed"
    elif have apt-get; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch
        [ $want_loader = 0 ] || set -- "$@" libvulkan1
        [ $want_vk = 0 ] || set -- "$@" mesa-vulkan-drivers
        root apt-get update
        root apt-get install -y "$@" || die "installing $* failed"
    elif have dnf; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch
        [ $want_loader = 0 ] || set -- "$@" vulkan-loader
        [ $want_vk = 0 ] || set -- "$@" mesa-vulkan-drivers
        root dnf install -y "$@" || die "installing $* failed"
    elif have zypper; then
        set --
        [ $want_ra = 0 ] || set -- "$@" retroarch
        [ $want_loader = 0 ] || set -- "$@" libvulkan1
        if [ $want_vk = 1 ]; then
            case $vendors in *0x1002*) set -- "$@" libvulkan_radeon ;; esac
            case $vendors in *0x8086*) set -- "$@" libvulkan_intel ;; esac
        fi
        [ $# -eq 0 ] || root zypper --non-interactive install "$@" || die "installing $* failed"
    else
        [ $want_ra = 0 ] || die "install RetroArch with your package manager, then run this again"
    fi
}

# SteamOS has a read-only system: RetroArch's AppImage goes in the prefix,
# without distribution packages or root. It runs as an ordinary program of
# the host: the host's graphics driver, /dev/kvm and controllers as they
# are, and a process and window Steam sees as the game it started.
have_pm() { have pacman || have apt-get || have dnf || have zypper; }
if [ -z "$rakind" ]; then
    if have retroarch; then rakind=system
    elif { [ "$os_id" = steamos ] || ! have_pm || [ $packages = 0 ]; } && [ "$arch" = x86_64 ]; then rakind=appimage
    else rakind=system
    fi
fi
retroarch=
case $rakind in
    system)
        if [ $packages = 1 ] && [ "$os_id" != steamos ]; then
            install_packages
        fi
        have retroarch || die "RetroArch isn't installed (retroarch is not in PATH); on AMD64 try --retroarch appimage"
        retroarch=$(command -v retroarch)
        ;;
    appimage)
        [ "$arch" = x86_64 ] || die "RetroArch's AppImage is AMD64 only; install RetroArch and use --retroarch system"
        ;;
esac
# The AppImage path is assigned after downloading it.
ra() { "$retroarch" "$@"; }
if [ -z "$(vulkan_icds)" ]; then
    case $(gpu_vendors) in
        *0x10de*) warn "no Vulkan driver found: install NVIDIA's driver (nvidia-utils or your distribution's equivalent)" ;;
        *) warn "no hardware Vulkan driver found; hakuX-lr needs Vulkan 1.1 or newer with timeline semaphores" ;;
    esac
fi
if loader=$(libvulkan); then
    # Read the version from the symlink target (libvulkan.so.1.3.275).
    lmin=$(readlink -f "$loader" | sed -n 's/.*libvulkan\.so\.1\.\([0-9][0-9]*\)\..*/\1/p')
    if [ -n "$lmin" ] && [ "$lmin" -lt 1 ]; then
        warn "the Vulkan loader is 1.$lmin; hakuX-lr requires a Vulkan 1.1 or newer loader"
    fi
else
    warn "libvulkan.so.1 (the Vulkan loader) is missing"
fi

# ------------------------------------------------------------- permissions

user=$(id -un)
groups_added=
add_group() { # <group> <why>
    [ "$(id -u)" -ne 0 ] || return 0
    getent group "$1" >/dev/null 2>&1 || return 0
    id -nG "$user" | tr ' ' '\n' | grep -qx "$1" && return 0
    if [ "$os_id" = steamos ]; then
        warn "$user is not in the $1 group ($2). Set a password with passwd if needed, then run: sudo usermod -aG $1 $user"
        return 0
    elif [ "$asroot" = none ]; then
        warn "$user is not in the $1 group ($2): as root, run usermod -aG $1 $user"
        return 0
    fi
    say "adding $user to the $1 group ($2)"
    if root usermod -aG "$1" "$user"; then
        groups_added="$groups_added $1"
    else
        warn "couldn't add $user to the $1 group ($2): run sudo usermod -aG $1 $user"
    fi
}
rw() { [ -r "$1" ] && [ -w "$1" ]; }

if [ "$arch" = x86_64 ]; then
    # The AMD64 core runs the game's code on the host CPU through KVM.
    if [ ! -e /dev/kvm ]; then
        if grep -qw svm /proc/cpuinfo; then kmod=kvm_amd
        elif grep -qw vmx /proc/cpuinfo; then kmod=kvm_intel
        else kmod=; fi
        if [ -n "$kmod" ] && [ "$asroot" != none ]; then
            root modprobe "$kmod" || true
        elif [ -n "$kmod" ] && [ "$os_id" = steamos ]; then
            warn "KVM is not loaded. Set a password with passwd if needed, then run: sudo modprobe $kmod"
        fi
        [ -e /dev/kvm ] || warn "/dev/kvm is missing: enable AMD SVM / Intel VT-x in the BIOS/UEFI, then load kvm_amd or kvm_intel"
    fi
    if [ -e /dev/kvm ] && ! rw /dev/kvm; then
        add_group "$(stat -c %G /dev/kvm)" "access to /dev/kvm"
    fi
    kmaj=$(uname -r | cut -d. -f1) kmin=$(uname -r | cut -d. -f2 | tr -cd 0-9)
    if [ "$kmaj" -lt 5 ] || { [ "$kmaj" -eq 5 ] && [ "${kmin:-0}" -lt 16 ]; }; then
        warn "Linux $(uname -r) is older than 5.16; the KVM backend needs 5.16 or newer"
    fi
    grep -qw umip /proc/cpuinfo || warn "the CPU reports no UMIP; the KVM backend needs it"
    if [ -r /sys/module/kvm_amd/parameters/tsc_scaling ]; then
        case $(cat /sys/module/kvm_amd/parameters/tsc_scaling) in
            1|Y) ;;
            *) warn "kvm_amd has TSC scaling off (needs Zen 2 or newer)" ;;
        esac
    elif grep -q '^vmx flags' /proc/cpuinfo && ! grep '^vmx flags' /proc/cpuinfo | grep -qw tsc_scaling; then
        warn "the CPU reports no VMX TSC scaling; the KVM backend needs it"
    fi
fi

# Without a display server, RetroArch draws through KMS and reads the
# keyboard and controllers from /dev/input. A console login gets access to
# both from logind; over SSH they need the video and input groups.
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    card=$(ls /dev/dri/card[0-9]* 2>/dev/null | head -n1)
    if [ -n "$card" ] && ! rw "$card"; then
        add_group "$(stat -c %G "$card")" "drawing through KMS"
    fi
    readable=0
    for ev in /dev/input/event*; do
        [ -r "$ev" ] && { readable=1; break; }
    done
    if [ $readable = 0 ] && [ -e /dev/input/event0 ]; then
        add_group "$(stat -c %G /dev/input/event0)" "keyboard and controllers on KMS"
    fi
fi

# -------------------------------------------------------------------- core

mkdir -p "$prefix" "$prefix/cores" "$prefix/info" "$prefix/saves" \
    "$prefix/states" "$prefix/system" "$prefix/screenshots" "$prefix/logs" \
    "$prefix/config" "$prefix/playlists" "$prefix/cache" "$prefix/autoconfig" \
    "$games"
prefix=$(CDPATH='' cd -- "$prefix" && pwd)
games=$(CDPATH='' cd -- "$games" && pwd)
if [ "$os_id" = steamos ] && have flock; then
    exec 9>"$prefix/.core-update.lock"
    flock 9
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hakux-lr-setup.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 130' INT TERM

# --------------------------------------------------- RetroArch's AppImage

if [ "$rakind" = appimage ]; then
    app="$prefix/retroarch/RetroArch.AppImage"
    # Ozone draws part of its icons from XMB's monochrome set (the sidebar's
    # last entries, the button hints, the clock's battery): without it they
    # are white boxes. An installation made before that set was unpacked is
    # unpacked again.
    if [ ! -x "$app" ] || [ "$(cat "$prefix/retroarch/version" 2>/dev/null)" != "$APPIMAGE_VERSION" ] ||
        [ ! -d "$prefix/retroarch/assets/xmb/monochrome" ]; then
        have bsdtar || have 7z || have 7za || die "bsdtar or 7z is needed to unpack RetroArch's AppImage"
        say "downloading RetroArch $APPIMAGE_VERSION (AppImage, 180 MB)"
        fetch "$APPIMAGE_URL" "$tmp/RetroArch.7z" || die "couldn't download $APPIMAGE_URL"
        got=$(sha256 "$tmp/RetroArch.7z")
        [ "$got" = "$APPIMAGE_SHA256" ] || die "checksum mismatch for RetroArch.7z: $got, expected $APPIMAGE_SHA256"
        mkdir "$tmp/ra"
        in=RetroArch-Linux-x86_64
        asset_root=$in/RetroArch-Linux-x86_64.AppImage.home/.config/retroarch
        if have bsdtar; then
            bsdtar -xf "$tmp/RetroArch.7z" -C "$tmp/ra" "$in/RetroArch-Linux-x86_64.AppImage" \
                "$asset_root/assets/ozone" "$asset_root/assets/xmb/monochrome" "$asset_root/assets/pkg" \
                "$asset_root/assets/sounds" || die "couldn't unpack RetroArch.7z"
        else
            z=$(command -v 7z || command -v 7za)
            "$z" x -y -o"$tmp/ra" "$tmp/RetroArch.7z" "$in/RetroArch-Linux-x86_64.AppImage" \
                "$asset_root/assets/ozone" "$asset_root/assets/xmb/monochrome" "$asset_root/assets/pkg" \
                "$asset_root/assets/sounds" >/dev/null || die "couldn't unpack RetroArch.7z"
        fi
        mkdir -p "$prefix/retroarch"
        rm -rf "$prefix/retroarch/assets" "$prefix/retroarch/squashfs-root"
        cp -R "$tmp/ra/$asset_root/assets" "$prefix/retroarch/assets"
        # Keep the AppImage separate from its bundled portable home folder.
        cp "$tmp/ra/$in/RetroArch-Linux-x86_64.AppImage" "$prefix/retroarch/.RetroArch.AppImage.new"
        chmod +x "$prefix/retroarch/.RetroArch.AppImage.new"
        mv -f "$prefix/retroarch/.RetroArch.AppImage.new" "$app"
        echo "$APPIMAGE_VERSION" > "$prefix/retroarch/version"
        rm -rf "$tmp/ra" "$tmp/RetroArch.7z"
    fi
    retroarch=$app
    if ! "$app" --version >/dev/null 2>&1; then
        # Fall back to extraction when FUSE mounting is unavailable.
        say "unpacking the AppImage (it couldn't be mounted)"
        (cd "$prefix/retroarch" && "$app" --appimage-extract >/dev/null 2>&1) ||
            die "RetroArch's AppImage doesn't run here; install RetroArch and use --retroarch system"
        retroarch="$prefix/retroarch/squashfs-root/AppRun"
        "$retroarch" --version >/dev/null 2>&1 ||
            die "RetroArch's AppImage doesn't run here; install RetroArch and use --retroarch system"
    fi
fi
if ! ra --features 2>/dev/null | grep -i vulkan | grep -q -i yes; then
    warn "RetroArch ($rakind) reports no Vulkan support; hakuX-lr needs RetroArch's vulkan driver"
fi

auto_version=
if [ -n "$corefile" ]; then
    say "installing $corefile"
    cp "$corefile" "$tmp/hakux-lr_libretro.so"
    info=${corefile%.so}.info
    if [ -f "$info" ]; then
        cp "$info" "$tmp/hakux-lr_libretro.info"
    elif [ -f "$prefix/info/hakux-lr_libretro.info" ]; then
        cp "$prefix/info/hakux-lr_libretro.info" "$tmp/hakux-lr_libretro.info"
    elif [ -f "$prefix/info/hakux_libretro.info" ]; then
        cp "$prefix/info/hakux_libretro.info" "$tmp/hakux-lr_libretro.info"
    else
        die "no hakux-lr_libretro.info next to $corefile; install a release first"
    fi
    installed="local build ($corefile)"
else
    if [ -z "$tarball" ]; then
        if [ -z "$version" ]; then
            say "looking up the newest hakuX-lr release for $arch"
            url=$(fetch_text "https://api.github.com/repos/$REPO/releases?per_page=50" |
                grep -Eo "https://github.com/$REPO/releases/download/linux-v[0-9][0-9.]*/hakux(-lr)?-[0-9][0-9.]*-$arch\.tar\.gz\"" |
                head -n1 | tr -d '"')
            [ -n "$url" ] || die "no $arch release found at https://github.com/$REPO/releases"
            tag=${url%/*}
            auto_version=${tag##*/linux-v}
        else
            url="https://github.com/$REPO/releases/download/linux-v$version/hakux-lr-$version-$arch.tar.gz"
        fi
        tarball="$tmp/${url##*/}"
        say "downloading ${url##*/}"
        if ! fetch "$url" "$tarball"; then
            [ -n "$version" ] || die "couldn't download $url"
            url="${url%/*}/hakux-$version-$arch.tar.gz"
            tarball="$tmp/${url##*/}"
            say "downloading ${url##*/}"
            fetch "$url" "$tarball" || die "couldn't download $url"
        fi
        fetch "$url.sha256" "$tarball.sha256" 2>/dev/null || die "couldn't download $url.sha256"
        want=$(cut -d' ' -f1 "$tarball.sha256")
        got=$(sha256 "$tarball")
        [ "$want" = "$got" ] || die "checksum mismatch for ${url##*/}: $got, expected $want"
    fi
    mkdir "$tmp/x"
    tar -xzf "$tarball" -C "$tmp/x"
    so=$(find "$tmp/x" -path '*/lib/libretro/hakux-lr_libretro.so' -print -quit)
    if [ -z "$so" ]; then
        so=$(find "$tmp/x" -path '*/lib/libretro/hakux_libretro.so' -print -quit)
    fi
    [ -n "$so" ] || die "$tarball holds no lib/libretro/hakux-lr_libretro.so"
    cp "$so" "$tmp/hakux-lr_libretro.so"
    cp "${so%.so}.info" "$tmp/hakux-lr_libretro.info"
    rel=${so%/lib/libretro/*}
    built=$(sed -n 's/^architecture //p' "$rel/BUILD-INFO" 2>/dev/null)
    [ -z "$built" ] || [ "$built" = "$arch" ] || die "${tarball##*/} is built for $built, this machine is $arch"
    installed=$(head -n1 "$rel/BUILD-INFO" 2>/dev/null || true)
    installed=${installed:-hakuX-lr (${tarball##*/})}
    licenses="$rel/share/licenses/hakux-lr"
    [ -d "$licenses" ] || licenses="$rel/share/licenses/hakux"
    rm -rf "$prefix/licenses"
    if [ -d "$licenses" ]; then
        cp -R "$licenses" "$prefix/licenses"
        cp "$rel/BUILD-INFO" "$prefix/licenses/" 2>/dev/null || true
    fi
fi
# Check the libc RetroArch and the core run with: the host's.
libc_version=$(ldd --version 2>/dev/null || true)
glibc=$(printf '%s\n' "$libc_version" | sed -n '1s/.* \([0-9][0-9]*\.[0-9][0-9]*\)$/\1/p')
need=$(LC_ALL=C grep -ao 'GLIBC_2\.[0-9][0-9]*' "$tmp/hakux-lr_libretro.so" | sort -u -t. -k2,2n | tail -n1)
need=${need#GLIBC_2.}
if [ -n "$glibc" ] && [ -n "$need" ]; then
    gmaj=${glibc%%.*} gmin=${glibc#*.}
    if [ "$gmaj" -lt 2 ] || { [ "$gmaj" -eq 2 ] && [ "$gmin" -lt "$need" ]; }; then
        warn "glibc $glibc is older than 2.$need, which this core needs; RetroArch won't load it"
    fi
fi

# Replace the files, never write into them: a running RetroArch keeps
# the old core mapped.
cp "$tmp/hakux-lr_libretro.so" "$prefix/cores/.hakux-lr_libretro.so.new"
mv -f "$prefix/cores/.hakux-lr_libretro.so.new" "$prefix/cores/hakux-lr_libretro.so"
cp "$tmp/hakux-lr_libretro.info" "$prefix/info/hakux-lr_libretro.info"
rm -f "$prefix/cores/hakux_libretro.so" "$prefix/info/hakux_libretro.info" "$prefix/info/core_info.cache"
say "installed $installed"

# RetroArch normally sorts saves by the core's reported name. Share the
# existing directory so an older release can still use the same Xbox disk.
if [ -d "$prefix/saves/hakuX" ] && [ ! -e "$prefix/saves/hakuX-lr" ] &&
    [ ! -L "$prefix/saves/hakuX-lr" ]; then
    ln -sT hakuX "$prefix/saves/hakuX-lr"
fi

# ------------------------------------------------------- controller profiles

say "downloading RetroArch's controller profiles"
if fetch "$AUTOCONFIG_URL" "$tmp/autoconfig.tar.gz" 2>/dev/null &&
    mkdir "$tmp/ac" && tar -xzf "$tmp/autoconfig.tar.gz" -C "$tmp/ac"; then
    udev=$(find "$tmp/ac" -maxdepth 2 -type d -name udev | head -n1)
    if [ -n "$udev" ]; then
        rm -rf "$prefix/autoconfig/udev"
        cp -R "$udev" "$prefix/autoconfig/udev"
    fi
else
    warn "couldn't download the controller profiles; map controllers in Settings → Input"
fi

# ------------------------------------------------------------ configuration

cfgval() { # <key> <file>: an uncommented value from a RetroArch config
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\"\{0,1\}\([^\"]*\)\"\{0,1\}[[:space:]]*$/\1/p" "$2" 2>/dev/null | tail -n1
}
# Our --config replaces RetroArch's defaults, so set its asset path explicitly.
assets=
case $rakind in
    appimage) assets="$prefix/retroarch/assets" ;;
    *)
        for sys in /etc/retroarch.cfg /usr/local/etc/retroarch.cfg; do
            v=$(cfgval assets_directory "$sys")
            [ -n "$v" ] && [ -d "$v" ] && { assets=$v; break; }
        done
        ;;
esac
if [ -z "$assets" ] && [ "$rakind" = system ]; then
    for d in /usr/share/libretro/assets /usr/share/retroarch/assets \
        /usr/local/share/libretro/assets /usr/local/share/retroarch/assets \
        "${XDG_CONFIG_HOME:-$HOME/.config}/retroarch/assets"; do
        if [ -d "$d/ozone" ] || [ -d "$d/xmb" ] || [ -d "$d/glui" ]; then
            assets=$d
            break
        fi
    done
fi
menu=rgui
if [ -n "$assets" ]; then
    if [ -d "$assets/ozone" ]; then menu=ozone
    elif [ -d "$assets/xmb" ]; then menu=xmb
    fi
fi

q() { printf '%s' "$1" | sed 's/"/\\"/g'; }
{
    cat <<EOF
# hakuX-lr's RetroArch configuration, written by setup-hakux-lr.sh: running it
# again rewrites this file. Put your own settings in user.cfg, which
# start-hakux-lr.sh loads after this one.
config_save_on_exit = "false"

libretro_directory = "$(q "$prefix")/cores"
libretro_info_path = "$(q "$prefix")/info"
core_options_path = "$(q "$prefix")/retroarch-core-options.cfg"
rgui_config_directory = "$(q "$prefix")/config"
input_remapping_directory = "$(q "$prefix")/config/remaps"
savefile_directory = "$(q "$prefix")/saves"
savestate_directory = "$(q "$prefix")/states"
system_directory = "$(q "$prefix")/system"
screenshot_directory = "$(q "$prefix")/screenshots"
playlist_directory = "$(q "$prefix")/playlists"
cache_directory = "$(q "$prefix")/cache"
joypad_autoconfig_dir = "$(q "$prefix")/autoconfig"
log_dir = "$(q "$prefix")/logs"
rgui_browser_directory = "$(q "$games")"
EOF
    [ -z "$assets" ] || printf 'assets_directory = "%s"\n' "$(q "$assets")"
    for sys in /etc/retroarch.cfg /usr/local/etc/retroarch.cfg; do
        [ "$rakind" = system ] || break
        [ -f "$sys" ] || continue
        for k in audio_filter_dir video_filter_dir video_shader_dir overlay_directory osk_overlay_directory; do
            v=$(cfgval "$k" "$sys")
            [ -n "$v" ] && [ -d "$v" ] && printf '%s = "%s"\n' "$k" "$(q "$v")"
        done
        break
    done
    cat <<EOF

# RetroArch writes its log file only with verbose logging on
log_verbosity = "true"
log_to_file = "true"
log_to_file_timestamp = "false"

# Use Vulkan and bilinear scaling as on PortareOS. The core paces frames.
video_driver = "vulkan"
video_fullscreen = "true"
video_vsync = "false"
video_threaded = "false"
video_smooth = "true"
menu_driver = "$menu"
# RetroArch's Qt desktop window: of no use here, and without a display
# server Qt aborts RetroArch once a game has loaded.
desktop_menu_enable = "false"
ui_companion_enable = "false"
ui_companion_start_on_boot = "false"
ui_companion_toggle = "false"

input_joypad_driver = "udev"
input_autodetect_enable = "true"
# Menu: the controller's guide button where its profile maps one, or hold
# L1 + R1 + Start + Select. Esc twice quits.
input_menu_toggle_gamepad_combo = "3"
quit_press_twice = "true"

# hakuX-lr supports no save states, so none of what needs them.
rewind_enable = "false"
run_ahead_enabled = "false"
preemptive_frames_enable = "false"
savestate_auto_save = "false"
savestate_auto_load = "false"
EOF
} > "$prefix/retroarch.cfg"

if [ ! -f "$prefix/user.cfg" ]; then
    cat > "$prefix/user.cfg" <<'EOF'
# Your RetroArch settings for hakuX-lr: start-hakux-lr.sh loads this file last,
# and setup-hakux-lr.sh never changes it. For example:
#   video_smooth = "false"
#   network_cmd_enable = "true"
EOF
fi

# ------------------------------------------------------------- the launcher

# An installation from when SteamOS got RetroArch as a Flatpak.
was_flatpak=0
! grep -qs "^RA_KIND='flatpak'" "$prefix/hakux-lr.env" || was_flatpak=1
printf "RETROARCH='%s'\nRA_KIND='%s'\nGAMES='%s'\nARCH='%s'\nOS_ID='%s'\nVIA_STEAM='%s'\n" \
    "$(printf '%s' "$retroarch" | sed "s/'/'\\\\''/g")" \
    "$rakind" \
    "$(printf '%s' "$games" | sed "s/'/'\\\\''/g")" \
    "$arch" "$(printf '%s' "$os_id" | tr -cd 'a-z0-9._-')" "$steam" > "$prefix/hakux-lr.env"

cat > "$prefix/start-hakux-lr.sh" <<'LAUNCHER'
#!/bin/sh
# Copyright (C) 2026-present PortareOS
# Starts RetroArch with hakuX-lr as setup-hakux-lr.sh set it up.
#
#   start-hakux-lr.sh [--windowed] [--verbose] [<game> | <part of its name>]
#
# A game is an .iso or .chd file, or a file name in the games folder, whole
# or in part, with or without its extension ("burnout"). Without one,
# RetroArch's menu opens: Load Content browses the games folder. Without a
# display server (no WAYLAND_DISPLAY or DISPLAY) RetroArch draws straight
# through KMS. HAKUX_DISPLAY=kms or desktop overrides that. The core's
# diagnostics go to logs/hakux-lr.log, RetroArch's to logs/retroarch.log.
#
# On SteamOS, desktop launches use the library shortcut for Steam Input.
# Terminal launches, --no-steam installs and HAKUX_VIA_STEAM=0 run directly.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd)
. "$here/hakux-lr.env"

die() { printf 'start-hakux-lr: %s\n' "$*" >&2; exit 1; }
warn() { printf 'start-hakux-lr: %s\n' "$*" >&2; }
usage() { sed -n '3,/^set -eu$/p' "$(readlink -f -- "$0")" | sed '$d; s/^# \{0,1\}//'; }

windowed=0 verbose=0
while [ $# -gt 0 ]; do
    case $1 in
        -w|--windowed) windowed=1 ;;
        -v|--verbose) verbose=1 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1 (see --help)" ;;
        *) break ;;
    esac
    shift
done
[ $# -le 1 ] || die "one game at a time (see --help)"

game=
if [ $# -eq 1 ]; then
    if [ -f "$1" ]; then
        game=$(readlink -f -- "$1")
    else
        # a name in the games folder, whole or in part, its extension
        # optional; the name's own wildcards literal, a space or underscore
        # matching either ("burnout 3": Burnout_3_-_Takedown)
        name=$1
        case $name in
            *.[iI][sS][oO]|*.[cC][hH][dD]) name=${name%.*} ;;
        esac
        pat=$(printf '%s' "$name" | sed 's/[][*?\\]/\\&/g' | tr ' _' '??')
        matches=$(find -L "$GAMES" -maxdepth 2 -type f \( -iname "*$pat*.iso" -o -iname "*$pat*.chd" \) 2>/dev/null | sort)
        n=$(printf '%s' "$matches" | grep -c . || true)
        if [ "$n" -gt 1 ]; then
            # the one whose whole name it is ("halo": Halo.iso, not Halo 2.iso)
            want=$(printf '%s' "$name" | tr 'A-Z _' 'a-z__')
            exact=$(printf '%s\n' "$matches" | while IFS= read -r m; do
                b=${m##*/}
                [ "$(printf '%s' "${b%.*}" | tr 'A-Z _' 'a-z__')" != "$want" ] || printf '%s\n' "$m"
            done)
            if [ "$(printf '%s' "$exact" | grep -c . || true)" = 1 ]; then
                matches=$exact n=1
            fi
        fi
        case $n in
            0) die "no game matching \"$1\" in $GAMES" ;;
            1) game=$matches ;;
            *) printf '%s\n' "$matches" >&2; die "\"$1\" matches $n games; be more specific" ;;
        esac
    fi
fi

# Find this launcher's shortcut in the active Steam profile. If there is no
# MostRecent entry, use the most recently modified profile. The binary
# shortcut record stores a four-byte app ID before its AppName.
steam_shortcut() {
    python3 -c '
import glob, os, re, sys
home = sys.argv[1]
roots = [r for r in (home + "/.steam/steam", home + "/.local/share/Steam") if os.path.isdir(r + "/userdata")]
for root in roots[:1]:
    account = None
    try:
        users = open(root + "/config/loginusers.vdf", errors="replace").read()
        for steamid, body in re.findall(r"\"(\d{17})\"\s*\{(.*?)\}", users, re.S):
            if re.search(r"\"MostRecent\"\s*\"1\"", body, re.I):
                account = int(steamid) - 76561197960265728
    except OSError:
        pass
    if account is not None:
        profiles = [root + "/userdata/" + str(account)]
    else:
        profiles = sorted(glob.glob(root + "/userdata/*"), key=os.path.getmtime, reverse=True)[:1]
    for profile in profiles:
        try:
            data = open(profile + "/config/shortcuts.vdf", "rb").read()
        except OSError:
            continue
        for m in re.finditer(rb"\x02appid\x00(.{4})\x01appname\x00hakuX-lr \(Xbox\)\x00", data, re.S | re.I):
            end = data.find(b"\x02appid\x00", m.end())
            rest = data[m.end():end if end >= 0 else len(data)].lower()
            if b"start-hakux" in rest or b"hakux-lr.desktop" in rest:
                print((int.from_bytes(m.group(1), "little") << 32) | 0x02000000)
                sys.exit(0)
' "$HOME" 2>/dev/null || true
}

# Show fallback instructions to desktop users.
tell() {
    warn "$1"
    if command -v kdialog >/dev/null 2>&1; then
        kdialog --title "hakuX-lr" --passivepopup "$1" 20 >/dev/null 2>&1 &
    elif command -v notify-send >/dev/null 2>&1; then
        notify-send -t 20000 "hakuX-lr" "$1" >/dev/null 2>&1 &
    fi
}
recover="The Deck's controls act as keyboard and mouse here. For a controller, start hakuX-lr (Xbox) from the Steam library (Non-Steam); if it isn't there: Steam, Add a Game, Add a Non-Steam Game."

by_steam=${SteamGameId:-}${SteamClientLaunch:-}
if [ -n "$by_steam" ] && [ -n "$(find "$here/handoff.late" -mmin -2 2>/dev/null)" ]; then
    # The fallback already started RetroArch. Ignore Steam's late launch.
    rm -f "$here/handoff.late"
    exit 0
fi
rm -f "$here/handoff.late"
if [ -n "$by_steam" ] && [ -f "$here/handoff" ]; then
    # Read the game and options, then acknowledge the Steam launch.
    {
        IFS= read -r handed || handed=
        IFS= read -r opts || opts=
    } < "$here/handoff"
    [ -n "$game" ] || game=$handed
    case " $opts " in *" windowed "*) windowed=1 ;; esac
    case " $opts " in *" verbose "*) verbose=1 ;; esac
    rm -f "$here/handoff"
    : > "$here/handoff.ack"
elif [ -z "$by_steam" ] && [ "${OS_ID:-}" = steamos ] && [ "${HAKUX_VIA_STEAM:-${VIA_STEAM:-1}}" != 0 ] && [ ! -t 2 ] &&
    { [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; }; then
    id=
    if command -v steam >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
        id=$(steam_shortcut)
    fi
    if [ -n "$id" ]; then
        opts=
        [ $windowed = 0 ] || opts="$opts windowed"
        [ $verbose = 0 ] || opts="$opts verbose"
        rm -f "$here/handoff.ack"
        printf '%s\n%s\n' "$game" "$opts" > "$here/handoff"
        steam "steam://rungameid/$id" >/dev/null 2>&1 &
        n=0
        while [ $n -lt "${HAKUX_STEAM_WAIT:-20}" ] && [ ! -f "$here/handoff.ack" ]; do
            sleep 1
            n=$((n + 1))
        done
        if [ -f "$here/handoff.ack" ]; then
            rm -f "$here/handoff.ack"
            exit 0
        fi
        # Mark the timeout so a late Steam launch does not open RetroArch.
        rm -f "$here/handoff"
        : > "$here/handoff.late"
        tell "Steam didn't start hakuX-lr in time, so RetroArch starts without it. $recover"
    else
        tell "hakuX-lr (Xbox) isn't in this Steam account's library, so RetroArch starts without Steam. $recover"
    fi
fi

mode=${HAKUX_DISPLAY:-}
if [ -z "$mode" ]; then
    if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then mode=desktop; else mode=kms; fi
fi

if [ "$ARCH" = x86_64 ]; then
    [ -e /dev/kvm ] || die "/dev/kvm is missing: enable virtualization (SVM / VT-x) and load kvm_amd or kvm_intel"
    { [ -r /dev/kvm ] && [ -w /dev/kvm ]; } ||
        die "no access to /dev/kvm: follow setup-hakux-lr.sh's permission instructions, then log out and in again"
fi

rt=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
if [ -S "$rt/pulse/native" ]; then audio=pulse
elif [ -S "$rt/pipewire-0" ]; then audio=pipewire
else audio=alsa; fi

update="$here/.core-update"
if [ "${HAKUX_AUTO_UPDATE:-1}" != 0 ] && [ -f "$update/version" ] &&
    command -v python3 >/dev/null 2>&1; then
    if [ -d "$update/pending" ]; then
        python3 "$update/update.py" apply "$here" "$ARCH" >>"$here/logs/update.log" 2>&1 || true
    fi
    python3 "$update/update.py" check "$here" "$ARCH" </dev/null >>"$here/logs/update.log" 2>&1 &
fi

session="$here/session.cfg"
{
    echo "# written by start-hakux-lr.sh for each start"
    echo "audio_driver = \"$audio\""
    if [ "$mode" = kms ]; then
        echo 'video_context_driver = "khr_display"'
        echo 'input_driver = "udev"'
        echo 'video_fullscreen = "true"'
        # The connected display's preferred mode, listed first; without
        # it RetroArch takes the largest (4096x2160 on a 3840x2160 TV).
        for st in /sys/class/drm/card*-*/status; do
            [ "$(cat "$st" 2>/dev/null)" = connected ] || continue
            m=$(head -n1 "${st%/status}/modes" 2>/dev/null)
            w=${m%%x*} h=${m#*x} h=${h%%[!0-9]*}
            case $w$h in ''|*[!0-9]*) continue ;; esac
            echo "video_fullscreen_x = \"$w\""
            echo "video_fullscreen_y = \"$h\""
            break
        done
    elif [ $windowed = 1 ]; then
        echo 'video_fullscreen = "false"'
    fi
} > "$session"

if [ "$mode" = kms ]; then
    card=
    for c in /dev/dri/card[0-9]*; do
        [ -r "$c" ] && [ -w "$c" ] && { card=$c; break; }
    done
    [ -n "$card" ] || die "no access to /dev/dri/card*: start from a console login, or log in again after setup-hakux-lr.sh added you to the video group"
    readable=0
    for ev in /dev/input/event*; do
        [ -r "$ev" ] && { readable=1; break; }
    done
    [ $readable = 1 ] || warn "no access to /dev/input: keyboard and controllers won't work (log in again, or start from a console login)"
    # Qt (RetroArch's desktop window, off in retroarch.cfg) aborts the
    # process when it finds no display server.
    export QT_QPA_PLATFORM=offscreen
fi

set -- --config "$here/retroarch.cfg" --appendconfig "$session|$here/user.cfg"
[ $verbose = 0 ] || set -- "$@" --verbose
[ -z "$game" ] || set -- "$@" -L "$here/cores/hakux-lr_libretro.so" "$game"
# hakuX-lr writes its diagnostics to stderr, which RetroArch's log file doesn't
# take: they get a file of their own (the last start's kept as hakux-lr.log.1)
corelog="$here/logs/hakux-lr.log"
mkdir -p "$here/logs"
[ ! -f "$corelog" ] || mv -f "$corelog" "$corelog.1"
echo "start-hakux-lr: ${game:-RetroArch menu} ($mode, $audio)"
echo "start-hakux-lr: logs: $corelog (the core), $here/logs/retroarch.log (RetroArch)"
exec "$RETROARCH" "$@" 2>"$corelog"
LAUNCHER
chmod +x "$prefix/start-hakux-lr.sh"

if [ "$os_id" = steamos ]; then
    update="$prefix/.core-update"
    rm -f "$update/version" "$update/checked"
    rm -rf "$update/pending"
    if [ "$auto_update" = 1 ] && have python3 && have flock &&
        printf '%s\n' "$auto_version" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        mkdir -p "$update"
        cat > "$update/update.py" <<'UPDATER'
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys
import tarfile
import tempfile
import time
from urllib.request import Request, urlopen


REPO = "portare-ch/hakuX-lr"
LATEST = f"https://api.github.com/repos/{REPO}/releases/latest"
CORE = "hakux-lr_libretro.so"
INFO = "hakux-lr_libretro.info"
CHECK_INTERVAL = 24 * 60 * 60


def version(value):
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", value):
        return tuple(map(int, value.split(".")))
    raise ValueError(f"invalid release version {value!r}")


def open_url(url):
    return urlopen(Request(url, headers={"User-Agent": "hakuX-lr-update"}), timeout=10)


def check(update, installed, arch):
    checked = update / "checked"
    if checked.exists() and time.time() - checked.stat().st_mtime < CHECK_INTERVAL:
        return
    checked.touch()
    with open_url(LATEST) as response:
        release = json.load(response)
    tag = release.get("tag_name", "")
    if release.get("draft") or release.get("prerelease") or not tag.startswith("linux-v"):
        return
    candidate = tag[len("linux-v"):]
    if version(candidate) <= version(installed):
        return
    folder = f"hakux-lr-{candidate}"
    name = f"{folder}-{arch}.tar.gz"
    assets = {asset["name"] for asset in release.get("assets", [])}
    if not {name, name + ".sha256"} <= assets:
        return
    url = f"https://github.com/{REPO}/releases/download/{tag}/{name}"
    with open_url(url + ".sha256") as response:
        checksum = response.read(256).decode("ascii").split()
    if not checksum or not re.fullmatch(r"[0-9a-fA-F]{64}", checksum[0]):
        raise ValueError("invalid release checksum")

    with tempfile.TemporaryDirectory(dir=update) as temporary:
        work = Path(temporary)
        archive = work / name
        digest = hashlib.sha256()
        with open_url(url) as response, archive.open("wb") as output:
            for chunk in iter(lambda: response.read(65536), b""):
                digest.update(chunk)
                output.write(chunk)
        if digest.hexdigest() != checksum[0].lower():
            raise ValueError(f"checksum mismatch for {name}")
        staged = work / "pending"
        staged.mkdir()
        with tarfile.open(archive, "r:gz") as package:
            for member, target in (
                (f"{folder}/lib/libretro/{CORE}", CORE),
                (f"{folder}/lib/libretro/{INFO}", INFO),
                (f"{folder}/BUILD-INFO", "BUILD-INFO"),
            ):
                entry = package.getmember(member)
                if not entry.isfile() or not entry.size:
                    raise ValueError(f"invalid package member {member}")
                with package.extractfile(entry) as source, (staged / target).open("wb") as output:
                    shutil.copyfileobj(source, output)
        build = (staged / "BUILD-INFO").read_text().splitlines()
        if build[:1] != [f"hakuX-lr {candidate}"] or f"architecture {arch}" not in build:
            raise ValueError("release manifest does not match the version or architecture")
        (staged / "version").write_text(candidate + "\n")
        staged.rename(update / "pending")
        print(f"Downloaded hakuX-lr {candidate} for the next launch")


def replace_file(source, destination):
    with tempfile.TemporaryDirectory(dir=destination.parent) as directory:
        temporary = Path(directory) / destination.name
        shutil.copyfile(source, temporary)
        os.replace(temporary, destination)


def apply(prefix, update, installed):
    pending = update / "pending"
    candidate = (pending / "version").read_text().strip()
    if version(candidate) > version(installed):
        info = prefix / "info" / INFO
        with tempfile.TemporaryDirectory(dir=update) as temporary:
            backup = Path(temporary) / INFO
            shutil.copyfile(info, backup)
            replace_file(pending / INFO, info)
            try:
                # RetroArch can keep reading an older core it has already mapped.
                replace_file(pending / CORE, prefix / "cores" / CORE)
            except OSError:
                replace_file(backup, info)
                raise
        (prefix / "info" / "core_info.cache").unlink(missing_ok=True)
        replace_file(pending / "BUILD-INFO", update / "BUILD-INFO")
        replace_file(pending / "version", update / "version")
        print(f"Installed hakuX-lr {candidate}")
    shutil.rmtree(pending)


def main():
    mode, directory, arch = sys.argv[1:]
    prefix = Path(directory)
    update = prefix / ".core-update"
    with (prefix / ".core-update.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        installed = (update / "version").read_text().strip()
        pending = (update / "pending").is_dir()
        if mode == "apply" and pending:
            apply(prefix, update, installed)
        elif mode == "check" and not pending:
            check(update, installed, arch)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        print(f"hakuX-lr update: {error}", file=sys.stderr)
UPDATER
        printf '%s\n' "$auto_version" > "$update/version"
    fi
    if have flock; then
        exec 9>&-
    fi
fi

# Existing Steam shortcuts and shell links still point to the old launcher.
if [ -f "$prefix/start-hakux.sh" ]; then
    ln -sfn start-hakux-lr.sh "$prefix/start-hakux.sh"
fi

mkdir -p "$HOME/.local/bin"
ln -sf "$prefix/start-hakux-lr.sh" "$HOME/.local/bin/start-hakux-lr.sh"

desktop=
if [ -n "${XDG_CURRENT_DESKTOP:-}" ] || [ "$os_id" = steamos ] ||
    ls /usr/share/wayland-sessions/*.desktop /usr/share/xsessions/*.desktop >/dev/null 2>&1; then
    apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
    mkdir -p "$apps"
    desktop="$apps/hakux-lr.desktop"
    icon=retroarch
    [ "$rakind" != appimage ] || icon=applications-games
    cat > "$desktop" <<EOF
[Desktop Entry]
Type=Application
Name=hakuX-lr (Xbox)
Comment=Original Xbox games in RetroArch
Exec="$prefix/start-hakux-lr.sh" %f
Icon=$icon
Terminal=false
Categories=Game;Emulator;
EOF
fi

# Send the shortcut to a running Steam client once, to avoid duplicates.
steam_added=
if [ $steam = 1 ] && [ "$os_id" = steamos ] && [ -n "$desktop" ]; then
    if [ -f "$prefix/steam-added" ]; then
        steam_added=before
    elif ! have steamos-add-to-steam || ! pgrep -x steam >/dev/null 2>&1; then
        steam_added=failed
    elif steamos-add-to-steam "$desktop" >/dev/null 2>&1; then
        : > "$prefix/steam-added"
        steam_added=now
    else
        steam_added=failed
    fi
fi

# ----------------------------------------------------------------- summary

case ":$PATH:" in
    *":$HOME/.local/bin:"*) launcher=start-hakux-lr.sh ;;
    *) launcher="$HOME/.local/bin/start-hakux-lr.sh" ;;
esac
echo
ra_version=$(ra --version 2>&1 | sed -n 's/^Version: *\([0-9][0-9.]*\).*/\1/p' | head -n1)
say "done: $installed with RetroArch ${ra_version:-(version unknown)} ($rakind)"
cat <<EOF
    games:     $games (.iso or .chd)
    start:     $launcher "<game or part of its name>"
    menu:      $launcher
    settings:  $prefix/user.cfg
    saves:     $prefix/saves (the Xbox hard disk is hdd/)
    logs:      $prefix/logs/hakux-lr.log (the core), retroarch.log (RetroArch)
EOF
if [ $was_flatpak = 1 ]; then
    echo
    echo "hakuX-lr no longer uses RetroArch's Flatpak. If nothing else needs it:"
    echo "    flatpak uninstall --user org.libretro.RetroArch"
fi
case $steam_added in
    now)
        echo
        echo "Sent hakuX-lr (Xbox) to Steam. Check Non-Steam in your library, then start it in"
        echo "Gaming Mode and choose Load Content to pick a game. In Desktop Mode, hakuX-lr (Xbox)"
        echo "in the application menu starts through Steam too."
        ;;
    failed)
        echo
        echo "Couldn't add hakuX-lr to Steam (is Steam running?). In Desktop Mode: Steam, Add a Game,"
        echo "Add a Non-Steam Game, hakuX-lr (Xbox)."
        ;;
esac
if [ "$arch" = x86_64 ] && [ -e /dev/kvm ] && ! rw /dev/kvm && [ -z "$groups_added" ]; then
    echo
    echo "No access to /dev/kvm yet: games won't start until this user can read and write it (see the warnings above)."
fi
if [ -n "$groups_added" ]; then
    echo
    echo "Log out and in again (or start a new SSH session) so the new groups apply:$groups_added"
fi
