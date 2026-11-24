# hakuX-lr

hakuX-lr is an original Xbox emulator built for RetroArch. It started as a fork
of the standalone Android emulator [hakuX](https://github.com/rfandango/hakuX)
and is maintained as a separate project. The compatibility list rates **753 game
entries** as **Playable** based on short gameplay tests on AMD64 Linux.

**[Download](https://github.com/portare-ch/hakuX-lr/releases)** · **[Game compatibility](https://hakuxlr.portareos.org/)** · **[Install](#install)**

## Features

- **KVM on AMD64 Linux.** Xbox x86 code runs directly on the host CPU. hakuX-lr
  corrects AMD reciprocal-square-root results to match the Xbox’s Pentium III.
  In the project’s comparison against an original Xbox, only KVM passed
  **all 15 CPU tests**. The Ryzen 7 9800X3D run matched **183 of 183 checks**.
  [CPU test results](https://hakuxlr.portareos.org/cputests/)
- **FEX on ARM64.** The FEX JIT translates Xbox x86 instructions into ARM64
  code for Linux and Android.
- **Vulkan graphics.** In the 0.34.0 AMD64 run, **3,016 of 3,556 NV2A tests**
  matched original Xbox captures within the suite’s image-comparison threshold.
  Render at 1×–4× native resolution and choose graphics accuracy independently,
  including Potato mode for lower-end handhelds. [Release results](https://github.com/portare-ch/hakuX-lr/releases/tag/linux-v0.34.0) · [Compare captures](https://hakuxlr.portareos.org/nv2atests/)
- **DSP JIT audio.** On both Linux architectures, **Full DSP (experimental)**
  uses a DSP56300 JIT to run Xbox audio effects, including reverb. Android
  defaults to the Fast mix and uses an interpreter for Full DSP.
- **ARM64 and AMD64 optimizations.** The ARM64 graphics decoder checks modified
  memory in batches. The AMD64 audio processor uses SSE4.1 for sample rounding.
  [CPU usage measurements](CHANGELOG.md#linux-v0340-linux-aarch64-and-x86-64-and-android)
- **Multiplayer.** Play over System Link or connect to Xbox Live through
  Insignia. The bundled dashboard handles console registration and account
  creation. [Multiplayer setup](USER_GUIDE.md#multiplayer)
- **Widescreen and 60 fps patches.** Enable patches individually in RetroArch’s
  **Cheats** menu. [Browse game patches](https://github.com/portare-ch/hakux-lr-patches)

<a id="how-hakux-compares-with-xemu"></a>
<a id="how-this-differs-from-upstream-hakux-and-xemu"></a>

## hakuX, hakuX-lr and xemu

| | [hakuX](https://github.com/rfandango/hakuX) | hakuX-lr | [xemu](https://xemu.app/) |
| --- | --- | --- | --- |
| Frontend | Standalone Android app | RetroArch core only | Desktop app and a separate [experimental libretro port](https://docs.libretro.com/library/xemu/) |
| Platforms | Android ARM64 | AMD64 and ARM64 Linux, Android ARM64 (experimental) | Windows, macOS and Linux. The libretro port also supports Android. |
| Xbox CPU | FEX JIT | KVM on AMD64 Linux, FEX on ARM64 | QEMU TCG by default |
| Graphics | Vulkan | Vulkan | Vulkan or OpenGL in the desktop app |
| Xbox kernel | Reimplemented | Reimplemented | Boots the Xbox kernel from the BIOS |
| Files needed to play | Your game image | Your game image | Your game image plus an [MCPX boot ROM, flash BIOS and hard disk image](https://xemu.app/docs/required-files/) |

## Install

Use your own Xbox XISO (`.iso`), full Redump ISO, or DVD CHD (`.chd`).
hakuX-lr creates its Xbox storage the first time you start a game.

<a id="play-on-portareos"></a>

| Platform | Build or setup |
| --- | --- |
| PortareOS on Retroid Pocket Nova | Bundled with [PortareOS](https://github.com/portare-ch/portareos/blob/next/documentation/INSTALLATION.md). Copy games to `/storage/roms/xbox/` and open **Xbox** in the launcher. |
| Steam Deck (SteamOS) | Use the [Steam Deck installer](#steam-deck). Games need read/write access to `/dev/kvm`. Installation on Deck hardware is untested. |
| Linux AMD64 (x86-64) | `hakux-lr-<version>-x86_64.tar.gz`. Requires Linux 5.16+, KVM with TSC scaling and UMIP, glibc 2.35+, and GCC 11’s libstdc++ or newer. |
| Linux ARM64 (AArch64) | `hakux-lr-<version>-aarch64.tar.gz`. Requires glibc 2.38+ and GCC 13’s libstdc++ or newer. |
| Android ARM64 (experimental) | `hakux-lr-<version>-android-arm64.zip`. Requires Android 9+ and 64-bit RetroArch. |
| ROCKNIX on supported ARM64 devices | On images that include hakuX-lr, put games in `roms/xbox/` and select the Xbox core. Older images list it as **hakux**. [Device core lists](https://github.com/ROCKNIX/distribution/tree/next/documentation/PER_DEVICE_DOCUMENTATION) |

Manual installs require 64-bit RetroArch and a Vulkan 1.1 or newer GPU driver
with timeline semaphores (`VK_KHR_timeline_semaphore` on 1.1). Vulkan 1.4 is
recommended. Linux also requires zlib and a Vulkan 1.1 or newer loader.

<a id="play-on-steam-deck"></a>

### Steam Deck

1. Switch to Desktop Mode and use **Save Link As** to download the [Steam Deck installer](https://raw.githubusercontent.com/portare-ch/hakuX-lr/master/tools/linux/hakux-lr-steamdeck.desktop).
2. In Dolphin, open the file’s **Properties → Permissions**, enable **Is executable**, then open the installer.
3. Copy games to the folder the installer prints. In Gaming Mode, open **Non-Steam → hakuX-lr (Xbox)** and choose **Load Content**.

<a id="play-on-other-aarch64-linux-systems"></a>
<a id="play-on-amd64-x86-64-linux-systems"></a>

### Linux

On AMD64, [set up KVM](USER_GUIDE.md#kvm-on-amd64) first. For an existing
RetroArch installation, download the release archive for your CPU.

1. Extract the archive. From `lib/libretro/`, copy `hakux-lr_libretro.so` to RetroArch’s **Cores** directory and `hakux-lr_libretro.info` to **Core Info**. Find both paths under **Settings → Directory**.
2. Set **Settings → Drivers → Video** to **vulkan**, save the current configuration, and restart RetroArch.
3. Choose **Load Core → Microsoft - Xbox (hakuX-lr)**, then **Load Content** and select your game.

If you already installed the core, follow the [upgrade guide](USER_GUIDE.md#upgrading-from-the-old-core-name).

For a fresh installation, the [Linux setup script](USER_GUIDE.md#linux-setup-script) can install RetroArch and the core together.

<a id="play-on-android"></a>

### Android ARM64 (experimental)

1. Extract the Android ZIP on the device. In RetroArch, select **Load Core → Install or Restore a Core** and pick `hakux-lr_libretro_android.so`.
2. Copy `hakux-lr_libretro.info` to the directory shown under **Settings → Directory → Core Info**.
3. Set **Settings → Drivers → Video** to **vulkan**, save the configuration, and restart RetroArch.
4. Choose **Load Core → Microsoft - Xbox (hakuX-lr)**, then **Load Content** and select your game.

## Development and license

[User guide](USER_GUIDE.md) · [Build for Linux](native/README.linux.md) · [Testing and bug reports](TESTING.md#reporting) · [Changelog](CHANGELOG.md)

hakuX-lr builds on the upstream hakuX contributors’ emulator, including their
port of xemu’s MCPX audio device. The combined core is distributed under
[GPL-3.0-or-later](COPYING). See [LICENSE](LICENSE) for individual component terms.
