# Linux build

The ARM64 Linux build provides `hakux_libretro.so` for RetroArch and the
diagnostic tools `xboxrun`, `vulkan-probe` and `kms-grab`. PortareOS uses the
RetroArch core; the untested standalone app was removed in 0.4.0.
See [TESTING.md](../TESTING.md) for installation and device testing.

## Build and check

Install Clang 19, CMake, Ninja, Git, Python 3, Rust 1.96.0, zlib development
files and Linux UAPI headers on an ARM64 Linux host. The supplied container
includes these tools. Dependencies are pinned in
[linux-deps.json](../tools/native/linux-deps.json), including FEX's required
submodules. The fetcher rejects modified or mismatched checkouts.

From the repository root:

```sh
python3 tools/native/linux-deps.py
CC=clang-19 CXX=clang++-19 sh tools/native/linux-build.sh
```

Dependencies go in `build/deps`; the Android `external/fex` checkout is
not needed.

On macOS with Apple's `container`, build the supplied image and run:

```sh
container build --tag hakux-linux --file tools/native/Dockerfile.linux tools/native
container run --rm --cpus 6 --memory 8g --volume "$PWD:/src" --workdir /src \
  hakux-linux sh -c 'python3 tools/native/linux-deps.py && sh tools/native/linux-build.sh'
```

Set `HAKUX_BUILD` to use another build directory, `HAKUX_DEPS` to select
dependencies fetched with `linux-deps.py --dest <directory>`, and `JOBS` to
set parallelism. Extra `linux-build.sh` arguments pass through to CMake,
including a sysroot or toolchain file.

The Linux ARM64 build enables `HAKUX_DSP_JIT` by default. It builds
dsp56300 0.1.3 from pinned sources using Cargo's locked dependencies;
release archives include their license notices and Rust toolchain version.
`-DHAKUX_DSP_JIT=OFF` builds the C interpreter without requiring Rust.

The build produces the core, diagnostic tools, `libretro-harness` and smoke
tests. CTest checks CPU/memory, audio, error handling and libretro behavior
using synthetic XBE/XISO files; no game dump or BIOS is needed. The separate
HLE tests below use Khronos headers from SDL3.

Run the pure HLE regression suite separately:

```sh
cmake -S tests/hle -B build/hle-tests -G Ninja -DCMAKE_C_COMPILER=clang-19 \
  -DNC_DEPS="$PWD/build/deps"
cmake --build build/hle-tests -j 4
ctest --test-dir build/hle-tests --output-on-failure
python3 tools/native/statelock.py
python3 tools/native/statelock.py --sizes --build build/linux
```

## Diagnostics on the Nova

Copy the executables and `tests/native/linux/check.py` to a dedicated
temporary directory on the device. Stripping copies with `llvm-strip-19`
reduces each core diagnostic from approximately 47 MiB to 6 MiB. Keep the
unstripped build on the development host for debugging.

Within the device's diagnostic directory:

```sh
timeout 60 python3 check.py .
timeout 15 ./vulkan-probe
ldd ./xboxrun
```

The suite checks guest arithmetic, branches, system-operation exits, kernel
initialization, shared RAM aliases and dirty-page/MMIO fault recovery. It
also checks expected failures: incorrect results, an unhandled host fault,
missing or invalid discs, and an infinite guest loop. A synthetic HLT title
boots in headless and render modes. The render test can pass without Vulkan;
verify actual drawing with a real game.

For a developer-supplied ISO/XISO, explicitly select headless mode and a
temporary data directory:

```sh
NC_TIMEOUT=30 ./xboxrun --headless /path/to/game.iso /tmp/hakux-diagnostic-data
```

Use a positive `NC_TIMEOUT`. Headless mode disables graphics and audio
output and stores the emulated disk and caches in the supplied directory.
Use `--render` to render without a display and capture frames to
`<data directory>/frames/` every 2 seconds (`NC_CAPTURE_EVERY`, limited by
`NC_CAPTURE_MAX`). Accepts XISO/full-dump `.iso` files and DVD CHDs.

`vulkan-probe` checks renderer requirements and creates a device. It does
not render, create a surface or verify shared-memory imports.

`kms-grab out.png` captures linear framebuffers, such as the launcher's.
For Turnip's UBWC-compressed swapchain images, it reports the size and
modifier without saving an image.

## The libretro core

Load `hakux_libretro.so` in RetroArch with its Vulkan video driver and open
an Xbox `.iso` or DVD `.chd` ([TESTING.md](../TESTING.md) has the steps). The
renderer shares RetroArch's Vulkan device and copies frames on the GPU; the
diagnostic harness uses CPU readback.

Ports 1–4 map to Xbox players 1–4. Setting a port's **Device Type** to
**None** disconnects it; changing it back reconnects it during play. Rumble
is available but off by default. Save states are unavailable; leave
run-ahead off. Closing, loading, resetting and switching game executables
work within the same RetroArch process. A video driver rebuild restarts
the game.

The PortareOS build (`HAKUX_LR_END_PROCESS`) exits RetroArch if shutdown
takes longer than 10 seconds. Set `HAKUX_LR_STOP=0` for the legacy behavior:
closing a game exits RetroArch, and switching game executables relaunches it.

Core options (`native/libretro/options.c`) apply on the next game start,
except Crop Overscan and Rumble, which update immediately:

| Key | Values | Default |
| --- | --- | --- |
| `hakux_render_scale` | `1`–`4` times the game's native resolution | `2` |
| `hakux_crop_overscan` | `enabled` (crop TV borders), `disabled` (full frame) | `enabled` |
| `hakux_screen_format` | `normal`, `widescreen` (shown 16:9), `letterbox` | `normal` |
| `hakux_language` | `auto` (RetroArch's), `english`, `japanese`, `german`, `french`, `spanish`, `italian`, `korean`, `chinese`, `portuguese` | `auto` |
| `hakux_video_region` | `auto` (the game's region), `ntsc-m`, `ntsc-j`, `pal` | `auto` |
| `hakux_rumble` | `disabled`, `enabled` (controller vibration) | `disabled` |
| `hakux_network` | `disabled` (no cable), `lan` (system link), `insignia` (Xbox Live through Insignia via NAT) | `disabled` |
| `hakux_audio_resampler` | `sinc` (higher quality, 32-tap filter using ARM NEON), `linear` (less CPU, softer high frequencies; fallback on non-ARM64 builds) | `sinc` |
| `hakux_audio_processing` | `fast` (dry mix), `dsp` (Xbox effects and stereo downmix; experimental) | `dsp` with JIT, otherwise `fast` |

Full DSP uses the DSP56300 JIT and is the default when compiled in. `NC_APU_DSP=0|1`
overrides the audio processing option; `NC_APU_DSP_JIT=0` selects the C
interpreter for debugging. Fast mode does not create JIT instances.

LAN mode (`native/net/lan.c`) sends Ethernet frames over UDP port 3075.
Discovery broadcasts on the default-route interface, or the first
broadcast-capable interface if needed. Add peers beyond broadcast range to
`lan-peers.txt` in the save directory: `IP` or `IP:PORT`, separated by
commas, spaces or newlines; `#` starts a comment. If two devices share a
console identity, RetroArch warns and generates a new serial number and
MAC address on the next game start. Existing saves remain valid.

Insignia mode uses upstream's NAT backend (`native/net/nat.c`, libslirp).
The console gets 10.0.2.15 via DHCP and uses Insignia's DNS server,
46.101.64.175. If your provider blocks that server, put an insigniaDNS
server's address in `insignia-dns.txt` in the save directory, using the same
format as `lan-peers.txt`. Only the first address is used. The core can't
boot the Xbox dashboard, so Insignia accounts can't be created or recovered
from RetroArch yet.

PAL60 is always enabled; 480p, 720p and 1080i are disabled. Use RetroArch's
shaders for filters such as FXAA and **Core provided** for the core's 4:3 or
16:9 aspect ratio.

The remaining design targets are in [LIBRETRO_SPEC.md](../LIBRETRO_SPEC.md).

`HAKUX_LR_STATS=1` logs time spent in `retro_run`. `HAKUX_DISP_STATS=1`
logs new frames per second, frame age and gaps between frames in any frontend.

## The PortareOS package

PortareOS's `hakux-lr` package installs the release tarball. Make it from a
clean checkout of the release commit, in the same image:

```sh
HAKUX_VERSION=0.12.0 CC=clang-19 CXX=clang++-19 sh tools/native/linux-build.sh
CC=clang-19 sh tools/native/linux-package.sh 0.12.0
```

This writes `build/package/hakux-0.12.0-aarch64.tar.gz` and prints its
SHA-256. The archive contains the stripped core and info file in
`lib/libretro/`, licenses in `share/licenses/hakux/`, and `BUILD-INFO` with
the source revision, compiler and dependency manifest. Sorted names, root
ownership and commit timestamps make packaging reproducible. Set the
PortareOS package's `PKG_VERSION` and `PKG_SHA256` to match the release.

`tools/native/linux-ci.sh <version>` fetches pinned sources, builds, runs
tests, packages and verifies the archive, checks generated shaders with
glslang, and validates the save-state schema. `linux-verify.py` also writes
the `.sha256` file. CI runs this in the pinned image on an ARM64 runner for
every push and keeps the package as an artifact.

To release, add a `## linux-v<version>` section to `CHANGELOG.md` on
`master` and push the tag `linux-v<version>` on that commit. CI builds the
tag and publishes the release with the section as its notes. A version with
a suffix, such as `0.12.0-exp.1`, is a pre-release that doesn't become the
latest release. The PortareOS bump (`PKG_VERSION`, `PKG_SHA256` from the
`.sha256` asset) is still a pull request there.

On PortareOS, console data is stored under `/storage/saves/xbox/`.

## Properties

Linux startup properties use names such as
`HAKUX_PROP_DEBUG_HAKUX_APUSPIN=0`. Existing `NC_*`/`HAKUX_*` environment
options remain available. Android properties still use the Android API.
Environment configuration does not provide live control of another process.

See [PORTAREOS_HANDOFF.md](../PORTAREOS_HANDOFF.md) for measured results,
outstanding validation and the next implementation steps.
