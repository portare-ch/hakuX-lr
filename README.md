# hakuX libretro core for aarch64 / amd64 devices and PortareOS

Play original Xbox games through RetroArch on the **Retroid Pocket Nova**
with **[PortareOS](https://github.com/portare-ch/portareos)**, which includes
hakuX. The ROCKNIX community has also tested the build on other devices.
For other Linux systems, install the core in RetroArch on
**[ARM64 (aarch64)](#play-on-other-aarch64-linux-systems)** or
**[AMD64 (x86-64)](#play-on-amd64-x86-64-linux-systems)**.

Bring your own game disc images. **No BIOS, MCPX ROM or hard disk image is
needed.** hakuX creates the console's storage on first launch.

## hakuX is free

hakuX is free, and always will be. Download the PortareOS Linux core from
[this fork's releases](https://github.com/portare-ch/hakuX-lr/releases), or the
Android app from [upstream's releases](https://github.com/rfandango/hakuX/releases).

There are no paid subscriptions, unlocks or "premium" builds. If someone
charged you for hakuX, request a refund and download it here for free.

## Play on PortareOS

1. **Install or update PortareOS.** Follow the
   [installation guide](https://github.com/portare-ch/portareos/blob/next/documentation/INSTALLATION.md)
   for a fresh installation. If you already use PortareOS, update through
   **Settings → About → Update**, then restart.
2. **Copy a game to `/storage/roms/xbox/`.** Use an Xbox `.iso` or a DVD
   `.chd`. Extract ZIP or 7z archives first; keep the disc image intact.
3. **Open Xbox in the launcher and select your game.** hakuX starts
   automatically. If a newly copied game is missing, press **Home** to
   return to the console list, then reopen Xbox.

To copy files over Wi-Fi, enable **Settings → SSH**. In an SFTP client,
connect to the IP address in **Settings → About**, with username `root`
and the password shown there. Copy games to `/storage/roms/xbox/`.

During a game, **Home + Start** returns to the launcher. **Home + the top
face button** opens RetroArch's Quick Menu for controls and core options.

<details>
<summary>Install a newer hakuX release before PortareOS includes it</summary>

1. Download `hakux-<version>-aarch64.tar.gz` from the latest `linux-v…`
   [release](https://github.com/portare-ch/hakuX-lr/releases) and extract it.
2. Inside the extracted folder, open `lib/libretro/`. Copy both
   `hakux_libretro.so` and `hakux_libretro.info` to `/storage/cores/` on the
   Nova, creating that directory if needed.
3. Restart the Nova, then launch a game from Xbox as usual.

These files override the bundled core even after OS updates. To restore
the bundled version, remove both and restart. Back up
`/storage/saves/xbox/hdd/` before changing versions; it holds your game saves.

</details>

## Play on other aarch64 Linux systems

Requires **64-bit ARM Linux**, glibc 2.38 or newer, libstdc++ from
GCC 13 or newer, zlib, and a **Vulkan 1.3** GPU driver. Install RetroArch
through your distribution.

1. Download and extract `hakux-<version>-aarch64.tar.gz` from the latest
   `linux-v…` [release](https://github.com/portare-ch/hakuX-lr/releases).
2. In RetroArch, open **Settings → Directory** and note the **Cores** and
   **Core Info** directories. From the archive's `lib/libretro/` folder,
   copy `hakux_libretro.so` into Cores and `hakux_libretro.info` into
   Core Info.
3. Set **Settings → Drivers → Video** to **vulkan**. Save the configuration
   through **Main Menu → Configuration File → Save Current Configuration**
   and restart RetroArch.
4. Choose **Load Core → Microsoft - Xbox (hakuX)**, then **Load Content**
   and select your game's `.iso` or `.chd` file.

Use the game's own save function. Save states, rewind and run-ahead are
unsupported; leave them off. hakuX stores game saves in `hdd/` under
RetroArch's save directory. Choose a writable directory under
**Settings → Directory → Save Files**.

## Play on AMD64 (x86-64) Linux systems

Requires **64-bit Intel or AMD Linux**, kernel 5.16 or newer, glibc 2.38 or
newer, libstdc++ from GCC 13 or newer, zlib, and a **Vulkan 1.3** GPU driver.
Install a 64-bit RetroArch through your distribution.

The AMD64 build runs Xbox code through **KVM**. The CPU and KVM must support
**TSC scaling** and **UMIP** (AMD Zen 2 or newer; Intel KVM can provide
UMIP). The core checks these features when starting a game and logs any
missing requirement. This build is experimental; automated KVM and core
tests pass, but real-game compatibility on AMD64 still needs testing.

1. **Enable virtualization** in BIOS/UEFI: **Intel Virtualization
   Technology (VT-x)** on Intel, or **SVM Mode (AMD-V)** on AMD.
2. **Load KVM** for your CPU: `sudo modprobe kvm_intel` on Intel, or
   `sudo modprobe kvm_amd` on AMD.
3. **Give your user access to `/dev/kvm`.** Check `ls -l /dev/kvm`. If its
   group is `kvm`, run the command below, then **log out and back in**:

   ```sh
   sudo usermod -aG kvm "$USER"
   ```

   Confirm that `test -r /dev/kvm && test -w /dev/kvm && echo 'KVM access OK'`
   prints the message. If RetroArch is sandboxed, it must also have access
   to `/dev/kvm`.
4. Download and extract **`hakux-<version>-x86_64.tar.gz`** from the latest
   `linux-v…` [release](https://github.com/portare-ch/hakuX-lr/releases).
   The `aarch64` archive is for ARM64 devices.
5. In RetroArch, open **Settings → Directory** and note the **Cores** and
   **Core Info** directories. From the archive's `lib/libretro/` folder,
   copy `hakux_libretro.so` into Cores and `hakux_libretro.info` into
   Core Info.
6. Set **Settings → Drivers → Video** to **vulkan**. Save through
   **Main Menu → Configuration File → Save Current Configuration**, then
   restart RetroArch.
7. Choose **Load Core → Microsoft - Xbox (hakuX)**, then **Load Content**
   and select your game's `.iso` or DVD `.chd` file.

AMD64 uses linear audio resampling and defaults to **Audio Processing →
Fast**; the ARM64 DSP JIT is unavailable. Use in-game saves in `hdd/` under
RetroArch's save directory. Save states, rewind and run-ahead are
unsupported; leave them off. See the
[testing guide](TESTING.md#is-it-fex-the-same-game-under-kvm) to compare a
game between AMD64 and ARM64.

## Game formats

Use an Xbox `.iso`: either an XISO or a full disc dump (Redump).

To save space, convert an ISO to CHD with `chdman` on your computer:

```sh
chdman createdvd -i "Game.iso" -o "Game.chd"
```

Copy `Game.chd` to your games folder; hakuX reads it directly (0.6.0 or
newer). Use **createdvd**. CD CHDs (`createcd`) and CHDs that need a parent
image are unsupported.

**BLACK** and **Burnout 3** have been tested on the Nova. Compatibility
varies; other games may have bugs or fail to start.

## Controls, picture, audio and saves

- **Controllers:** RetroArch ports 1–4 are Xbox players 1–4. Face buttons
  follow their Xbox positions. **L1 is White**, **R1 is Black**, and
  **L2/R2 are the triggers**. Remap through **Quick Menu → Controls**.
  **Rumble** is off by default: turn it on in **Quick Menu → Core
  Options**.
- **Picture:** open **Quick Menu → Core Options**. **Render Scale** is
  **2x** by default. Higher values sharpen the picture but need more GPU
  power; try **1x** if a game runs slowly.
  Set **Screen Format: Widescreen** for games that support it, and use
  RetroArch's **Core provided** aspect ratio. Close and reopen the game
  after changing render scale, screen format, language, video region or
  network. **Crop Overscan** changes immediately.
- **Audio:** on ARM64, **Audio Resampler** defaults to **Sinc**, which
  preserves more high-frequency detail and uses ARM NEON. Try **Linear**
  if audio crackles or runs slowly, then close and reopen the game. AMD64
  always uses linear resampling. **Audio Processing** defaults to
  **Full DSP (experimental)** in ARM64 Linux releases, running the
  game's Xbox reverb and other effects with a DSP JIT and stereo output.
  Select **Fast** for a dry mix that uses less CPU if a game has audio or
  performance problems. AMD64 defaults to **Fast**.
  Close and reopen the game after changing it.
- **System link:** to play a game's LAN mode across devices, set
  **Network** to **LAN (system link)** in **Quick Menu → Core Options** on
  each device, then close and reopen the game. Devices on the same Wi-Fi
  discover each other over UDP port 3075. For devices on another subnet or
  a VPN (Tailscale, ZeroTier), add their addresses to `lan-peers.txt`
  in the save directory
  (`/storage/saves/xbox/lan-peers.txt` on PortareOS): one per line, as `IP`
  or `IP:PORT`. XLink Kai is unavailable in RetroArch.
- **Xbox Live (Insignia):** set **Network** to **Xbox Live (Insignia)** and
  restart the game to connect to [Insignia](https://insignia.live), the
  revival of the original Xbox Live service. Register the console first
  by running Insignia's Setup Assistant as a game. For account creation
  or recovery, copy your Xbox dashboard files (`xboxdash.xbe` and its
  folders) into `hdd/Partition2/` under RetroArch's save directory
  (`/storage/saves/xbox/hdd/Partition2/` on PortareOS). Close the game,
  select **Load Core → Microsoft - Xbox (hakuX) → Start Core**, and open
  **Xbox Live** in the dashboard. Dashboard files are not included.
  Dashboard navigation and end-to-end Insignia setup still need device
  validation. If your provider blocks Insignia's DNS server, run
  insigniaDNS on another computer and put its address in `insignia-dns.txt`
  in the save directory (`/storage/saves/xbox/insignia-dns.txt` on
  PortareOS).
  For **Open NAT**, hakuX forwards the console's UDP 3074 through the
  device's UDP 3074 and asks the router to forward it through UPnP. Enable
  UPnP on the router or forward UDP 3074 to the device manually. If the
  device has a firewall, allow inbound UDP 3074 so peers can reach the
  console. Only one console behind the same router can use external UDP
  3074. Advanced overrides: `natport=`, `upnp=` and `upnplease=`; see
  [NAT options](native/README.linux.md#insignia-nat).
- **Saving:** save inside the game, as on an Xbox. On PortareOS, your saves
  live in `/storage/saves/xbox/hdd/`. Back up that folder to keep them when
  moving or reinstalling. RetroArch save states are not available.

## If something goes wrong

| Problem | What to try |
| --- | --- |
| Xbox or a game is missing from the PortareOS launcher | Check that the file is in `/storage/roms/xbox/` and ends in `.iso` or `.chd`. Extract ZIP/7z archives. Return to the console list with Home. Update PortareOS if CHDs are missing. |
| RetroArch cannot find hakuX | Check that both release files were copied into the directories above, then restart. |
| The game is slow or the picture is black | On other Linux systems, select the **vulkan** video driver and restart RetroArch. Try **Render Scale: 1x**. |
| A game will not start on AMD64 | Check virtualization in BIOS/UEFI, your read/write access to `/dev/kvm`, and the log for missing TSC scaling or UMIP support. |
| The game asks for a controller | Open **Quick Menu → Controls → Port 1 Controls** and set **Device Type** to **Xbox Controller**. A port set to None is unplugged. |
| A game fails to start | Try an XISO image and the latest core. For a CHD, check that it was made with `createdvd`. If it still fails, report it below. |

Still stuck? [Open an issue](https://github.com/portare-ch/hakuX-lr/issues)
with the game and region, hakuX version, device and OS, and what happened.
On PortareOS, copy `/var/log/exec.log` and
`/var/log/retroarch/retroarch.log` **before starting another game**.
[The testing guide](TESTING.md#logs) explains collecting logs in more detail.

## Development and credits

[Build for Linux](native/README.linux.md) · [Testing](TESTING.md) ·
[Changelog](CHANGELOG.md)

This is the PortareOS Linux fork of
[rfandango/hakuX](https://github.com/rfandango/hakuX), whose authors made
the emulator core and Android app. For Android, see the upstream project;
this fork's [Android build notes](android/README.md) are retained for
development and are not validated here.

GNU GPL v3 or later ([COPYING](COPYING)). Third-party code retains its own
licenses; see [LICENSE](LICENSE) and [licenses/](licenses/).
