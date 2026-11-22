# hakuX-lr user guide

[Installation and release downloads](README.md#install) · [Troubleshooting](#troubleshooting)

## KVM on AMD64

The x86-64 build needs Linux 5.16 or newer, TSC scaling, UMIP, and read/write
access to `/dev/kvm`. AMD Zen 2 and newer CPUs meet the CPU requirements.
On Intel, KVM can provide UMIP in software.

1. Enable Intel VT-x or AMD SVM/AMD-V in BIOS or UEFI.
2. Load the host’s KVM driver with `sudo modprobe kvm_intel` or `sudo modprobe kvm_amd`.
3. Run `ls -l /dev/kvm`. If the device belongs to the `kvm` group, add your user
   with `sudo usermod -aG kvm "$USER"`, then log out and back in.
4. Confirm access from the account that runs RetroArch.

   ```sh
   test -r /dev/kvm && test -w /dev/kvm && echo 'KVM access OK'
   ```

A sandboxed RetroArch installation also needs access to the device. The core
log reports missing KVM features. On Ubuntu 22.04, check `uname -r` first.
The original 5.15 kernel needs an upgrade. To install Ubuntu’s HWE kernel,
run `sudo apt install --install-recommends linux-generic-hwe-22.04` and reboot.

## Linux setup script

The script installs RetroArch and the hakuX-lr release for your CPU, sets up
a separate RetroArch configuration, and checks KVM access on AMD64. It supports
apt, dnf, pacman and zypper. `--retroarch flatpak` selects Flatpak.
`--retroarch appimage` selects the AMD64 AppImage and needs `bsdtar`, `7z` or `7za`.

```sh
curl -fsSLO https://raw.githubusercontent.com/portare-ch/hakuX-lr/master/tools/linux/setup-hakux-lr.sh
sh setup-hakux-lr.sh
```

Put games in `~/roms/xbox` or the games folder printed by the installer.
Run `~/.local/bin/start-hakux-lr.sh` to open RetroArch. Pass a file path or part
of a game’s name to start it directly. The launcher uses the desktop session
when available and KMS for direct display on a system without a desktop.

Run the installer again to update. `--version`, `--tarball` and `--core` select
a specific release or local build. Run `sh setup-hakux-lr.sh --help` for all
options. Settings and logs default to `~/.local/share/hakux-lr/`. Place
personal RetroArch settings in its `user.cfg` to carry them across updates.

### Steam Deck

The [Steam Deck installer](README.md#steam-deck) runs the same setup script
in Desktop Mode. To use the terminal, run the two commands above in Konsole.
On SteamOS the script uses RetroArch from your path when available. Otherwise
it uses the user Flatpak, installing it if needed. It looks for EmuDeck’s `Emulation/roms/xbox` directory,
including on SD cards, and falls back to `~/roms/xbox`.

- Hold **L1 + R1 + Start + Select** for RetroArch’s in-game menu.
- In Desktop Mode, **hakuX-lr (Xbox)** in the application menu starts through Steam when the signed-in Steam account has the hakuX-lr shortcut, so the Deck’s controls work as a controller. If the shortcut is missing or Steam doesn’t start it within 20 seconds, RetroArch starts directly and a notification says so; the Deck’s controls then act as Desktop Mode’s keyboard and mouse, as they do for a start from Konsole or with `HAKUX_VIA_STEAM=0`. Start it from the Steam library instead, or add the shortcut as described below.
- To add a missing Steam shortcut, choose **Steam → Add a Game → Add a Non-Steam Game → hakuX-lr (Xbox)** in Desktop Mode.
- If a game fails to launch, run `~/.local/bin/start-hakux-lr.sh` in Konsole to see the error. Check the installer output for KVM access instructions.
- To uninstall, back up your saves, remove `~/.local/share/hakux-lr`, `~/.local/bin/start-hakux-lr.sh` and `~/.local/share/applications/hakux-lr.desktop`, then remove the shortcut from Steam.

## Upgrading from the old core name

Releases through 0.34.0 used `hakux-*` archives and appeared as **Microsoft - Xbox (hakuX)**.
The [Linux setup script](#linux-setup-script) accepts both archive names and
keeps its installed launcher and sorted save directory accessible after
the upgrade. If you installed the core manually, back up
`hdd/` in RetroArch's save directory, install the new core and matching info
file, then remove the old core entry.

If your manual install sorts saves by core name, copy your existing
`hdd/` from the `hakuX` save folder to `hakuX-lr`. To carry over per-core overrides, options or
remaps, copy the corresponding `hakuX` folders under RetroArch's config and
remap directories to `hakuX-lr`, and rename the core-specific files inside
from `hakuX.cfg`, `hakuX.opt` or `hakuX.rmp` to the matching `hakuX-lr` names.
Game-specific filenames stay the same. Set **hakuX-lr** as the default core
again for any Xbox playlist that used the old entry.

## PortareOS

To update PortareOS, choose **Settings → About → Update** and restart. To
transfer games over Wi-Fi, enable **Settings → SSH**. Connect over SFTP as
`root` using the IP address and password under **Settings → About**.

To override the bundled core, back up your [saves](#saves) and extract the
ARM64 Linux release. Copy `hakux-lr_libretro.so` and `hakux-lr_libretro.info` from
`lib/libretro/` to `/storage/cores/` on the Nova, creating the directory if
needed, then restart. This override also applies after OS updates. To use the
bundled core again, remove both files and restart.

On PortareOS builds that still ship `/usr/lib/libretro/hakux_libretro.so`,
name the files `hakux_libretro.so` and `hakux_libretro.info` when copying them
to `/storage/cores/`. Copy `hakuX/hakuX.cfg` under
`/storage/.config/retroarch/config/` to `hakuX-lr/hakuX-lr.cfg` under the same
directory. This preserves `/storage/saves/xbox` and keeps run-ahead off.

## Android (experimental)

For the full core name and file associations in RetroArch, copy
`hakux-lr_libretro.info` from the Android ZIP to **Settings → Directory → Core Info**.
Android uses the Fast mix by default. Selecting **Full DSP (experimental)**
runs the effects through an interpreter.

## Game formats

hakuX-lr reads Xbox XISO images (`.iso`), full Redump ISOs and standalone
DVD CHDs (`.chd`). Extract ZIP or 7z archives before loading a game.

To compress a full Redump ISO, convert it to XISO first. This removes the
non-game partitions and padding before compression. The [converter](tools/iso_to_chd.py)
runs `xdvdfs pack` followed by `chdman createdvd`. Install
[xdvdfs-cli](https://github.com/antangelo/xdvdfs) and [chdman](https://docs.mamedev.org/tools/chdman.html) first.

```sh
python3 tools/iso_to_chd.py "Game.iso"
```

Add `--keep-xiso` to keep the intermediate XISO. The script preserves the
source ISO and refuses to overwrite existing files. Keep the original dump
for archival use. For a CHD made elsewhere, use a self-contained DVD image
created with `createdvd`.

## Controls

RetroArch ports 1–4 correspond to Xbox players 1–4. L1 maps to White, R1 to
Black, and L2/R2 to the Xbox triggers. Change mappings under **Quick Menu →
Controls**. To disconnect a port, set its **Device Type** to **None**. Enable
vibration under **Quick Menu → Core Options → Rumble**.

On PortareOS, **Home + Start** returns to the launcher and **Home + the top
face button** opens the RetroArch Quick Menu.

## Picture and audio

Open **Quick Menu → Core Options**. Reopen the game after changing settings.
Crop Overscan, Rumble and Frame Pacing take effect immediately.

| Option | Behavior |
| --- | --- |
| Render Scale | Defaults to 2×. Use 1× for native resolution or up to 4× for a sharper picture. |
| Screen Format | **Auto** selects 16:9 for games known to support widescreen, unless the frontend reports a display narrower than 16:10, and while a widescreen patch is active; 4:3 otherwise. A frontend that does not report the display's size (RetroArch 1.22.2 and older) counts as wide: select **Normal** on a 4:3 display there. Select **Widescreen** for a game with native 16:9 support that Auto does not know. Set RetroArch’s aspect ratio to **Core provided**. |
| Crop Overscan | Enabled by default to hide borders. Disable it to show the whole frame. |
| Accuracy Level | **Custom** uses the selected Combiner Precision. **Potato**, **Medium** and **High** apply the graphics settings below. Render scale is independent. |
| Combiner Precision | Used with **Custom**. Defaults to **Accurate** on AMD64 and **Fast** on ARM64. Fast skips 9-bit combiner rounding. |
| Audio Processing | **Auto** selects **Full DSP (experimental)** on Linux and **Fast** at Potato accuracy, on a Steam Deck or on Android. Fast plays the dry mix and reduces CPU use. |
| Audio Resampler | **Sinc** is the default. **Linear** reduces CPU use at some cost to audio quality. |
| Console Memory | Defaults to 64 MB. Choose 128 MB or 256 MB for homebrew and games that can use the extra RAM. |
| Patch Games for Console Memory | Off by default. Enable it to remove a game’s 64 MB limit when extra RAM is selected. The patch applies in memory. Compatibility varies by game. |
| Frame Pacing (experimental) | Off by default. **On** paces the game to the GPU. **Lowest Latency** waits one frame less and can reduce the frame rate. |

| Graphics behavior | Potato | Medium | High |
| --- | --- | --- | --- |
| Combiner rounding | Fast | Accurate | Accurate |
| Signed blend equations | Unsigned approximation | Accurate | Accurate |
| Dithering after blending into 16-bit targets | Host approximation | Host approximation | Accurate |

**Medium** is recommended for general play. **Potato** can change colors and
effects in games that use these features. **High** uses each accuracy path
the host GPU supports. The log identifies any fallback.

## Saves

Use the game’s own save menu. Saves live in `hdd/` under RetroArch’s **Save
Files** directory, or `/storage/saves/xbox/hdd/` on PortareOS. Choose a
writable save directory and back up `hdd/` when updating, reinstalling or
moving devices. Leave RetroArch save states, rewind and run-ahead disabled.

## Multiplayer

**Quick Menu → Core Options → Network** enables System Link and Xbox Live
together and is on by default. To disconnect, turn Network off and reopen
the game.

- **System Link.** Enable Network on each device and use the game’s System Link menu. Discovery uses UDP port 3075. For peers on other subnets or a VPN, put one `IP` or `IP:PORT` per line in `lan-peers.txt` in RetroArch’s save directory (`/storage/saves/xbox/` on PortareOS).
- **Insignia.** Load hakuX-lr and choose **Start Core** to open the bundled dashboard. Under **Xbox Live**, select **Register this console**, then **Create an Insignia account** using the subscription code from [Insignia](https://insignia.live).

Applied cheats, XBE patches and uncapped frame rates disable Xbox Live for
the session. System Link remains available. To reconnect to Insignia, turn
off the change and restart the game. [NAT and port forwarding](native/README.linux.md#insignia-nat)

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| Game missing on PortareOS | Use `/storage/roms/xbox/`, extract archives, and return to the console list with **Home**. Update PortareOS if CHDs are missing. |
| Core missing in RetroArch | Check **Cores** and **Core Info** under **Settings → Directory**, copy the release files to the matching directories, and restart RetroArch. |
| Slow game or black picture | Select the **vulkan** video driver and restart RetroArch. Try **Render Scale** at 1× and **Audio Processing** on **Fast**. |
| Game fails to launch on AMD64 | Check [KVM setup](#kvm-on-amd64) and look in the log for missing TSC scaling or UMIP. |
| Game asks for a controller | Choose **Quick Menu → Controls → Port 1 Controls → Device Type → Xbox Controller**. |
| Disc fails to load | Try the latest core and an XISO. For a CHD, check that it was created as a DVD image. |

When [reporting an issue](https://github.com/portare-ch/hakuX-lr/issues),
include the game and region, core version, device, OS, and steps to reproduce.
Collect the logs before starting another game.

| Installation | Logs |
| --- | --- |
| PortareOS | `/var/log/exec.log` and `/var/log/retroarch/retroarch.log` |
| Linux setup script or Steam Deck installer | `hakux-lr.log` and `retroarch.log` under `~/.local/share/hakux-lr/logs/` by default |
| Manual Linux installation | [Capture RetroArch’s terminal output](TESTING.md#logs-1) |
| Android | `adb logcat`, with tags starting with `hakuX-lr` |
