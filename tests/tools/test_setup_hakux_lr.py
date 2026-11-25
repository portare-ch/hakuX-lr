# SPDX-License-Identifier: GPL-2.0-or-later
"""Installer and launcher regressions using local cores and fake external tools."""
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/linux/setup-hakux-lr.sh"
DESKTOP = ROOT / "tools/linux/hakux-lr-steamdeck.desktop"
FAKE_TOOL = r'''#!PYTHON
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["CALL_LOG"], "a") as log:
    log.write(json.dumps([name, *args]) + "\n")
if name == "sed":
    if args[-1] == "/etc/os-release":
        print(os.environ.get("TEST_OS", "steamos"))
    else:
        os.execv("/usr/bin/sed", ["sed", *args])
elif name == "uname":
    print({"-m": os.environ.get("TEST_ARCH", "aarch64"), "-s": "Linux",
           "-r": "6.12.0"}[args[0]])
elif name == "id":
    print({"-u": "1000", "-un": "deck", "-nG": "users"}[args[0]])
elif name == "grep":
    if args == ["-qw", "svm", "/proc/cpuinfo"]:
        sys.exit(0)
    os.execv("/usr/bin/grep", ["grep", *args])
elif name == "ldd":
    print("ldd (GNU libc) 2.17")
elif name == "retroarch":
    if "--features" in args:
        print("Vulkan: yes")
    elif "--version" in args:
        print("Version: 1.22.2")
    else:
        sys.exit(int(os.environ.get("RETROARCH_RESULT", "0")))
elif name == "pgrep":
    sys.exit(0 if os.environ.get("STEAM_RUNNING", "1") == "1" else 1)
elif name == "steamos-add-to-steam":
    sys.exit(int(os.environ.get("STEAM_RESULT", "0")))
elif name == "kdialog":
    pass
elif name == "steam":
    # Simulate the Steam-launched instance acknowledging the handoff.
    if os.environ.get("STEAM_STARTS", "1") == "1":
        prefix = Path(os.environ["HOME"]) / ".local/share/hakux-lr"
        Path(os.environ["HANDOFF_SEEN"]).write_text((prefix / "handoff").read_text())
        (prefix / "handoff.ack").touch()
elif name == "curl" and "TEST_ARCHIVE" in os.environ:
    archive = Path(os.environ["TEST_ARCHIVE"])
    url = args[-1]
    if url.startswith("https://api.github.com/repos/portare-ch/hakuX-lr/releases?"):
        tag = "linux-v" + archive.name.rsplit("-", 2)[-2]
        print(json.dumps([{"assets": [{"browser_download_url":
            f"https://github.com/portare-ch/hakuX-lr/releases/download/{tag}/{archive.name}"}]}]))
    else:
        requested = url.rsplit("/", 1)[-1]
        if requested not in (archive.name, archive.name + ".sha256"):
            sys.exit(22)
        Path(args[args.index("-o") + 1]).write_bytes(archive.with_name(requested).read_bytes())
else:
    # No network, package installation or elevated commands in these tests.
    sys.exit(97)
'''


class SetupTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.home = self.root / "home with spaces"
        self.home.mkdir()
        self.prefix = self.home / ".local/share/hakux-lr"
        self.bindir = self.root / "bin"
        self.bindir.mkdir()
        self.log = self.root / "calls.jsonl"
        # RetroArch in PATH is the one used, on SteamOS too: the tests' stand-in.
        for name in ("sed", "uname", "id", "grep", "ldd", "retroarch", "curl",
                     "sudo", "pacman", "pgrep", "steamos-add-to-steam"):
            tool = self.bindir / name
            tool.write_text(FAKE_TOOL.replace("PYTHON", sys.executable, 1))
            tool.chmod(0o755)
        # Exclude host package managers and RetroArch from auto-detection.
        for name in ("sh", "tr", "head", "cut", "ls", "sort", "readlink", "stat",
                     "getent", "cat", "mkdir", "mktemp", "rm", "cp", "mv", "find",
                     "tail", "ln", "chmod", "dirname", "tar", "gzip", "sha256sum",
                     "flock", "python3"):
            (self.bindir / name).symlink_to(shutil.which(name))
        self.env = dict(os.environ, HOME=str(self.home),
                        PATH=str(self.bindir),
                        CALL_LOG=str(self.log), DISPLAY=":test", WAYLAND_DISPLAY="",
                        XDG_CURRENT_DESKTOP="KDE", XDG_DATA_HOME=str(self.home / ".local/share"))
        self.core = self.root / "hakux-lr_libretro.so"
        self.core.write_bytes(b"test core GLIBC_2.39\0")
        self.core.with_suffix(".info").write_text('corename = "hakuX-lr"\n')

    def install(self, *args, local_core=True, **env):
        command = ["sh", str(SCRIPT)]
        if local_core:
            command.extend(["--core", str(self.core)])
        result = subprocess.run([*command, *args],
                                env=dict(self.env, **env), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def archive(self, project, version, core_name):
        release = self.root / f"{project}-{version}"
        core = release / f"lib/libretro/{project}_libretro.so"
        core.parent.mkdir(parents=True)
        core.write_bytes(self.core.read_bytes())
        core.with_suffix(".info").write_text(f'corename = "{core_name}"\n')
        licenses = release / f"share/licenses/{project}"
        licenses.mkdir(parents=True)
        (licenses / "LICENSE").write_text("license\n")
        (release / "BUILD-INFO").write_text(f"{core_name} {version}\narchitecture aarch64\n")
        archive = release.with_name(release.name + "-aarch64.tar.gz")
        with tarfile.open(archive, "w:gz") as package:
            package.add(release, arcname=release.name)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        archive.with_name(archive.name + ".sha256").write_text(f"{digest}  {archive.name}\n")
        return archive

    def calls(self, name):
        return [call[1:] for line in self.log.read_text().splitlines()
                if (call := json.loads(line))[0] == name]

    def test_steamos_needs_no_root(self):
        # Force the missing-KVM path even when the test host has /dev/kvm.
        if Path("/dev/kvm").exists():
            self.skipTest("requires a host without /dev/kvm")
        result = self.install(TEST_ARCH="x86_64")
        self.assertFalse(self.calls("sudo"))
        self.assertFalse(self.calls("pacman"))
        self.assertIn("sudo modprobe kvm_amd", result.stderr)

    def run_setup(self, *args, **env):
        return subprocess.run(["sh", str(SCRIPT), "--core", str(self.core), *args],
                              env=dict(self.env, **env), capture_output=True, text=True)

    def test_steamos_without_retroarch_takes_the_appimage(self):
        (self.bindir / "retroarch").unlink()
        result = self.run_setup(TEST_ARCH="x86_64")
        # (no unpacker or network here: as far as wanting them for it)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("RetroArch's AppImage", result.stderr)
        self.assertFalse(self.calls("sudo"))
        self.assertFalse(self.calls("pacman"))

    def test_flatpak_is_no_longer_taken(self):
        result = self.run_setup("--retroarch", "flatpak")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no longer supported", result.stderr)
        # an installation made with it is told what it leaves behind
        self.prefix.mkdir(parents=True)
        (self.prefix / "hakux-lr.env").write_text("RETROARCH=''\nRA_KIND='flatpak'\n")
        result = self.install()
        self.assertIn("flatpak uninstall --user org.libretro.RetroArch", result.stdout)
        self.assertIn("RA_KIND='system'", (self.prefix / "hakux-lr.env").read_text())
        self.assertNotIn("flatpak uninstall", self.install().stdout)

    def test_system_retroarch_still_checks_host_libc(self):
        result = self.install("--no-packages", TEST_OS="arch")
        self.assertIn("glibc 2.17 is older", result.stderr)
        self.assertIn("RA_KIND='system'", (self.prefix / "hakux-lr.env").read_text())

    def test_steam_shortcut_is_added_once(self):
        self.install()
        self.install()
        self.assertEqual(len(self.calls("steamos-add-to-steam")), 1)
        self.assertTrue((self.prefix / "steam-added").exists())

    def test_steam_not_running_can_be_retried(self):
        self.install(STEAM_RUNNING="0")
        self.assertFalse(self.calls("steamos-add-to-steam"))
        self.assertFalse((self.prefix / "steam-added").exists())
        self.install()
        self.assertEqual(len(self.calls("steamos-add-to-steam")), 1)

    def test_failed_steam_request_can_be_retried(self):
        self.install(STEAM_RESULT="1")
        self.assertFalse((self.prefix / "steam-added").exists())
        self.install()
        self.assertTrue((self.prefix / "steam-added").exists())

    def test_low_latency_is_an_option(self):
        self.install("--no-steam")
        config = (self.prefix / "retroarch.cfg").read_text()
        for key in ("video_max_swapchain_images", "audio_latency", "vrr_runloop_enable"):
            self.assertNotIn(key, config)
        self.install("--no-steam", "--low-latency")
        config = (self.prefix / "retroarch.cfg").read_text()
        self.assertIn('video_max_swapchain_images = "2"', config)
        self.assertIn('audio_latency = "24"', config)
        self.assertIn('vrr_runloop_enable = "true"', config)
        # the helper that turns variable refresh rate on straight on the screen
        helper = self.prefix / "vrr.py"
        compile(helper.read_text(), str(helper), "exec")
        self.assertIn('"$here/vrr.py" "$c" 1', (self.prefix / "start-hakux-lr.sh").read_text())

    def kms_start(self, *setup, user=None, result=0):
        """A start straight on the screen, vrr.py a stand-in that notes its calls."""
        if not any(os.access(card, os.R_OK | os.W_OK) for card in Path("/dev/dri").glob("card[0-9]*")):
            self.skipTest("requires access to a /dev/dri card")
        self.install("--no-steam", *setup)
        noted = self.root / "vrr-calls"
        (self.prefix / "vrr.py").write_text(
            f"import sys\nopen({str(noted)!r}, 'a').write(sys.argv[2])\n")
        if user:
            (self.prefix / "user.cfg").write_text(user)
        started = subprocess.run([str(self.prefix / "start-hakux-lr.sh")],
                                 env=dict(self.env, HAKUX_DISPLAY="kms", RETROARCH_RESULT=str(result)),
                                 capture_output=True, text=True)
        return started.returncode, noted.read_text() if noted.exists() else ""

    def test_kms_start_turns_vrr_on_and_off_again(self):
        cards = len([c for c in Path("/dev/dri").glob("card[0-9]*")])
        self.assertEqual(self.kms_start("--low-latency"), (0, "1" * cards + "0" * cards))

    def test_kms_start_turns_vrr_off_after_a_failed_retroarch(self):
        cards = len([c for c in Path("/dev/dri").glob("card[0-9]*")])
        self.assertEqual(self.kms_start("--low-latency", result=3), (3, "1" * cards + "0" * cards))

    def test_kms_start_leaves_vrr_alone_unless_asked(self):
        self.assertEqual(self.kms_start(), (0, ""))
        self.assertEqual(self.kms_start("--low-latency", user='vrr_runloop_enable = "false"\n'), (0, ""))
        self.assertEqual(self.kms_start(user='vrr_runloop_enable = "true"\n')[1][:1], "1")

    def test_no_steam_option(self):
        self.install("--no-steam")
        self.assertFalse(self.calls("steamos-add-to-steam"))

    def test_launcher_follows_game_symlink(self):
        self.install()
        game = self.root / "Halo.iso"
        game.touch()
        games = self.home / "roms/xbox"
        (games / "Halo.iso").symlink_to(game)
        result = subprocess.run([str(self.prefix / "start-hakux-lr.sh"), "halo"],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(str(games / "Halo.iso"), self.calls("retroarch")[-1])
        self.assertIn(str(self.prefix / "cores/hakux-lr_libretro.so"), self.calls("retroarch")[-1])

    def steam_shortcut(self, appid=0x9ABCDEF0, account=42, exe=None, recent=True):
        """A shortcuts.vdf as Steam writes it, with another shortcut before ours."""
        exe = exe or f'"{self.prefix}/start-hakux-lr.sh"'
        def entry(index, appid, name, exe):
            return (b"\x00" + str(index).encode() + b"\x00\x02appid\x00" + appid.to_bytes(4, "little") +
                    b"\x01AppName\x00" + name + b"\x00\x01Exe\x00" + exe.encode() + b"\x00\x08")
        steam = self.home / ".steam/steam"
        config = steam / f"userdata/{account}/config"
        config.mkdir(parents=True)
        (config / "shortcuts.vdf").write_bytes(
            b"\x00shortcuts\x00" + entry(0, 0x11223344, b"Other", '"/x"') +
            entry(1, appid, b"hakuX-lr (Xbox)", exe) + b"\x08\x08")
        if recent:
            (steam / "config").mkdir(exist_ok=True)
            (steam / "config/loginusers.vdf").write_text(
                '"users"\n{\n\t"76561197960265729"\n\t{\n\t\t"MostRecent"\t\t"0"\n\t}\n'
                f'\t"{76561197960265728 + account}"\n\t{{\n\t\t"AccountName"\t\t"deck"\n\t\t"MostRecent"\t\t"1"\n\t}}\n}}\n')
        for name in ("python3", "sleep"):
            if not (self.bindir / name).exists():
                (self.bindir / name).symlink_to(shutil.which(name))
        for name in ("steam", "kdialog"):
            tool = self.bindir / name
            tool.write_text(FAKE_TOOL.replace("PYTHON", sys.executable, 1))
            tool.chmod(0o755)
        return (appid << 32) | 0x02000000

    def launch(self, *args, **env):
        result = subprocess.run([str(self.prefix / "start-hakux-lr.sh"), *args],
                                env=dict(self.env, HANDOFF_SEEN=str(self.root / "handoff-seen"), **env),
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_steamos_desktop_start_goes_through_steam(self):
        self.install()
        gameid = self.steam_shortcut()
        game = self.home / "roms/xbox/Halo.iso"
        game.touch()
        self.log.write_text("")
        self.launch("--windowed", "--verbose", "halo")
        self.assertEqual(self.calls("steam"), [[f"steam://rungameid/{gameid}"]])
        self.assertEqual((self.root / "handoff-seen").read_text(), f"{game}\n windowed verbose\n")
        self.assertFalse(self.calls("retroarch"))
        self.assertFalse(self.calls("kdialog"))
        self.assertFalse((self.prefix / "handoff.ack").exists())

    def test_steam_started_launcher_takes_the_handed_game_and_options(self):
        self.install()
        self.steam_shortcut()
        game = self.home / "roms/xbox/Halo.iso"
        game.touch()
        (self.prefix / "handoff").write_text(f"{game}\n windowed verbose\n")
        self.log.write_text("")
        self.launch(SteamGameId="1")
        self.assertFalse(self.calls("steam"))
        self.assertIn(str(game), self.calls("retroarch")[-1])
        self.assertIn("--verbose", self.calls("retroarch")[-1])
        self.assertIn('video_fullscreen = "false"', (self.prefix / "session.cfg").read_text())
        self.assertTrue((self.prefix / "handoff.ack").exists())
        self.assertFalse((self.prefix / "handoff").exists())

    def test_steam_not_starting_falls_back_and_a_late_start_ends(self):
        self.install()
        self.steam_shortcut()
        self.log.write_text("")
        result = self.launch(STEAM_STARTS="0", HAKUX_STEAM_WAIT="1")
        self.assertTrue(self.calls("steam"))
        self.assertEqual(len(self.calls("retroarch")), 1)
        self.assertIn("in time", result.stderr)
        self.assertIn("Add a Non-Steam Game", self.calls("kdialog")[-1][-2])
        self.assertFalse((self.prefix / "handoff").exists())
        # A late Steam launch exits after the direct fallback.
        self.launch(SteamGameId="1")
        self.assertEqual(len(self.calls("retroarch")), 1)
        # The next Steam library launch works normally.
        self.launch(SteamGameId="1")
        self.assertEqual(len(self.calls("retroarch")), 2)

    def test_shortcut_is_the_signed_in_accounts_and_this_launchers(self):
        self.install()
        self.steam_shortcut(appid=0x80000001, account=1, recent=False)
        gameid = self.steam_shortcut(appid=0x80000042, account=42)
        self.log.write_text("")
        self.launch()
        self.assertEqual(self.calls("steam"), [[f"steam://rungameid/{gameid}"]])
        # Reject a same-name shortcut that points to a different executable.
        shutil.rmtree(self.home / ".steam")
        self.steam_shortcut(exe='"/somewhere/else"')
        self.log.write_text("")
        result = self.launch()
        self.assertFalse(self.calls("steam"))
        self.assertTrue(self.calls("retroarch"))
        self.assertIn("isn't in this Steam account's library", result.stderr)
        self.assertTrue(self.calls("kdialog"))

    def test_direct_start_when_asked_or_not_steamos(self):
        self.install()
        self.steam_shortcut()
        self.log.write_text("")
        self.launch(HAKUX_VIA_STEAM="0")
        self.assertFalse(self.calls("steam"))
        self.assertFalse(self.calls("kdialog"))
        self.assertTrue(self.calls("retroarch"))
        env_file = self.prefix / "hakux-lr.env"
        self.assertIn("OS_ID='steamos'", env_file.read_text())
        env_file.write_text(env_file.read_text().replace("OS_ID='steamos'", "OS_ID='arch'"))
        self.log.write_text("")
        self.launch()
        self.assertFalse(self.calls("steam"))
        self.install("--no-steam")
        self.log.write_text("")
        result = self.launch()
        self.assertFalse(self.calls("steam"))
        self.assertNotIn("Steam", result.stderr)

    def test_latest_release_handles_both_archive_names(self):
        for project, version, core_name in (("hakux", "0.34.0", "hakuX"), ("hakux-lr", "0.35.0", "hakuX-lr")):
            with self.subTest(project=project):
                archive = self.archive(project, version, core_name)
                self.install(local_core=False, TEST_ARCHIVE=str(archive))
                self.assertEqual((self.prefix / "cores/hakux-lr_libretro.so").read_bytes(), self.core.read_bytes())
                self.assertIn(f'corename = "{core_name}"', (self.prefix / "info/hakux-lr_libretro.info").read_text())
                self.assertTrue((self.prefix / "licenses/LICENSE").is_file())

    def test_explicit_old_release_falls_back_to_legacy_filename(self):
        archive = self.archive("hakux", "0.34.0", "hakuX")
        self.install("--version", "0.34.0", local_core=False, TEST_ARCHIVE=str(archive))
        downloads = [args[-1].rsplit("/", 1)[-1] for args in self.calls("curl")]
        self.assertIn("hakux-lr-0.34.0-aarch64.tar.gz", downloads)
        self.assertIn("hakux-0.34.0-aarch64.tar.gz", downloads)

    def test_steam_deck_stages_stable_update_and_applies_it_on_next_start(self):
        initial = self.archive("hakux-lr", "0.37.0", "hakuX-lr")
        self.install(local_core=False, TEST_ARCHIVE=str(initial))
        update = self.prefix / ".core-update"
        self.assertEqual((update / "version").read_text(), "0.37.0\n")
        settings = self.prefix / "user.cfg"
        settings.write_text('video_smooth = "false"\n')
        core = self.prefix / "cores/hakux-lr_libretro.so"
        original = core.read_bytes()
        updated = b"updated test core GLIBC_2.39\0"
        self.core.write_bytes(updated)
        archive = self.archive("hakux-lr", "0.38.0", "hakuX-lr")
        updater = runpy.run_path(str(update / "update.py"))
        latest = updater["LATEST"]
        url = "https://github.com/portare-ch/hakuX-lr/releases/download/linux-v0.38.0/"
        metadata = json.dumps({
            "tag_name": "linux-v0.38.0",
            "assets": [{"name": archive.name}, {"name": archive.name + ".sha256"}],
        }).encode()
        replies = {latest: metadata, url + archive.name: archive.read_bytes()}
        checksum_url = url + archive.name + ".sha256"
        replies[checksum_url] = b"0" * 64
        check = updater["check"]
        with patch.dict(check.__globals__, open_url=lambda url: io.BytesIO(replies[url])):
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                check(update, "0.37.0", "aarch64")
            self.assertFalse((update / "pending").exists())
            self.assertEqual(core.read_bytes(), original)
            checked = update / "checked"
            checked_at = checked.stat().st_mtime
            replies[checksum_url] = archive.with_name(archive.name + ".sha256").read_bytes()
            before_interval = checked_at - 23 * 60 * 60
            os.utime(checked, (before_interval, before_interval))
            check(update, "0.37.0", "aarch64")
            self.assertFalse((update / "pending").exists())
            after_interval = checked_at - 25 * 60 * 60
            os.utime(checked, (after_interval, after_interval))
            check(update, "0.37.0", "aarch64")
        self.assertEqual(core.read_bytes(), original)
        self.assertTrue((update / "pending").is_dir())

        with core.open("rb") as old_core:
            self.launch(SteamGameId="1")
            self.assertEqual(old_core.read(), original)
        self.assertEqual(core.read_bytes(), updated)
        self.assertEqual((update / "version").read_text(), "0.38.0\n")
        self.assertFalse((update / "pending").exists())
        self.assertEqual(settings.read_text(), 'video_smooth = "false"\n')

    def test_automatic_core_updates_leave_pinned_and_local_builds_alone(self):
        archive = self.archive("hakux-lr", "0.37.0", "hakuX-lr")
        env = {"local_core": False, "TEST_ARCHIVE": str(archive)}
        version = self.prefix / ".core-update/version"
        self.install(**env)
        self.assertTrue(version.exists())
        self.install("--version", "0.37.0", **env)
        self.assertFalse(version.exists())
        self.install("--no-auto-update", **env)
        self.assertFalse(version.exists())
        self.install(**env)
        self.install()
        self.assertFalse(version.exists())

    def test_update_preserves_saves_and_prior_launcher(self):
        self.install()
        old_launcher = self.prefix / "start-hakux.sh"
        (self.prefix / "start-hakux-lr.sh").rename(old_launcher)
        old_core = self.prefix / "cores/hakux_libretro.so"
        (self.prefix / "cores/hakux-lr_libretro.so").rename(old_core)
        old_info = self.prefix / "info/hakux_libretro.info"
        (self.prefix / "info/hakux-lr_libretro.info").rename(old_info)
        save = self.prefix / "saves/hakuX/hdd/Partition1/save.dat"
        save.parent.mkdir(parents=True)
        save.write_bytes(b"Xbox save")

        self.install()
        self.assertEqual(save.read_bytes(), b"Xbox save")
        self.assertEqual((self.prefix / "saves/hakuX-lr/hdd/Partition1/save.dat").read_bytes(), b"Xbox save")
        self.assertFalse(old_core.exists())
        self.assertFalse(old_info.exists())
        self.assertEqual(old_launcher.resolve(), self.prefix / "start-hakux-lr.sh")
        self.assertEqual(len(self.calls("steamos-add-to-steam")), 1)

    def test_update_keeps_an_existing_new_save_directory(self):
        previous = self.prefix / "saves/hakuX"
        current = self.prefix / "saves/hakuX-lr"
        previous.mkdir(parents=True)
        current.mkdir()
        self.install()
        self.assertTrue(current.is_dir())
        self.assertFalse(current.is_symlink())

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-utils not installed")
    def test_desktop_entries_are_valid(self):
        self.install()
        generated = self.home / ".local/share/applications/hakux-lr.desktop"
        result = subprocess.run(["desktop-file-validate", str(DESKTOP), str(generated)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
