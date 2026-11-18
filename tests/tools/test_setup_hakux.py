# SPDX-License-Identifier: GPL-2.0-or-later
"""Installer and launcher regressions using local cores and fake external tools."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/linux/setup-hakux.sh"
DESKTOP = ROOT / "tools/linux/hakux-steamdeck.desktop"
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
elif name == "flatpak":
    if args[0] == "info":
        sys.exit(0 if os.environ.get("FLATPAK_INSTALLED", "1") == "1" else 1)
    if args[:2] == ["run", "--command=sh"]:
        print("/app/share/libretro/assets")
    elif args[:2] == ["run", "--command=ldd"]:
        print("ldd (GNU libc) 2.40")
    elif "--features" in args:
        print("Vulkan: yes")
    elif "--version" in args:
        print("Version: 1.22.2")
elif name == "retroarch":
    if "--features" in args:
        print("Vulkan: yes")
    elif "--version" in args:
        print("Version: 1.22.2")
elif name == "pgrep":
    sys.exit(0 if os.environ.get("STEAM_RUNNING", "1") == "1" else 1)
elif name == "steamos-add-to-steam":
    sys.exit(int(os.environ.get("STEAM_RESULT", "0")))
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
        for name in ("sed", "uname", "id", "grep", "ldd", "flatpak", "curl",
                     "sudo", "pacman", "pgrep", "steamos-add-to-steam"):
            tool = self.bindir / name
            tool.write_text(FAKE_TOOL.replace("PYTHON", sys.executable, 1))
            tool.chmod(0o755)
        # Exclude host package managers and RetroArch from auto-detection.
        for name in ("sh", "tr", "head", "cut", "ls", "sort", "readlink", "stat",
                     "getent", "cat", "mkdir", "mktemp", "rm", "cp", "mv", "find",
                     "tail", "ln", "chmod", "dirname"):
            (self.bindir / name).symlink_to(shutil.which(name))
        self.env = dict(os.environ, HOME=str(self.home),
                        PATH=str(self.bindir),
                        CALL_LOG=str(self.log), DISPLAY=":test", WAYLAND_DISPLAY="",
                        XDG_CURRENT_DESKTOP="KDE", XDG_DATA_HOME=str(self.home / ".local/share"))
        self.core = self.root / "hakux_libretro.so"
        self.core.write_bytes(b"test core GLIBC_2.39\0")
        self.core.with_suffix(".info").write_text('display_name = "hakuX"\n')

    def install(self, *args, **env):
        result = subprocess.run(["sh", str(SCRIPT), "--core", str(self.core), *args],
                                env=dict(self.env, **env), capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def calls(self, name):
        return [call[1:] for line in self.log.read_text().splitlines()
                if (call := json.loads(line))[0] == name]

    def test_steamos_uses_flatpak_without_root(self):
        # Force the missing-KVM path even when the test host has /dev/kvm.
        if Path("/dev/kvm").exists():
            self.skipTest("requires a host without /dev/kvm")
        result = self.install(TEST_ARCH="x86_64")
        self.assertFalse(self.calls("sudo"))
        self.assertFalse(self.calls("pacman"))
        self.assertIn("sudo modprobe kvm_amd", result.stderr)
        self.assertIn("RA_KIND='flatpak'", (self.prefix / "hakux.env").read_text())

    def test_flatpak_checks_runtime_libc(self):
        result = self.install()
        self.assertNotIn("glibc 2.17 is older", result.stderr)
        self.assertIn(["run", "--command=ldd", "org.libretro.RetroArch", "--version"],
                      self.calls("flatpak"))
        self.assertFalse(self.calls("ldd"))
        cfg = (self.prefix / "retroarch.cfg").read_text()
        self.assertIn('assets_directory = "/app/share/libretro/assets"', cfg)
        self.assertIn('menu_driver = "ozone"', cfg)

    def test_new_flatpak_is_installed_for_user(self):
        self.install(FLATPAK_INSTALLED="0")
        self.assertIn(["install", "--user", "--noninteractive", "flathub", "org.libretro.RetroArch"],
                      self.calls("flatpak"))

    def test_system_retroarch_still_checks_host_libc(self):
        tool = self.bindir / "retroarch"
        tool.write_text(FAKE_TOOL.replace("PYTHON", sys.executable, 1))
        tool.chmod(0o755)
        result = self.install("--no-packages", TEST_OS="arch")
        self.assertIn("glibc 2.17 is older", result.stderr)
        self.assertIn("RA_KIND='system'", (self.prefix / "hakux.env").read_text())
        self.assertFalse(self.calls("flatpak"))

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

    def test_no_steam_option(self):
        self.install("--no-steam")
        self.assertFalse(self.calls("steamos-add-to-steam"))

    def test_launcher_follows_game_symlink(self):
        self.install()
        game = self.root / "Halo.iso"
        game.touch()
        games = self.home / "roms/xbox"
        (games / "Halo.iso").symlink_to(game)
        result = subprocess.run([str(self.prefix / "start-hakux.sh"), "halo"],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(str(games / "Halo.iso"), self.calls("flatpak")[-1])
        self.assertIn("-L", self.calls("flatpak")[-1])

    @unittest.skipUnless(shutil.which("desktop-file-validate"), "desktop-file-utils not installed")
    def test_desktop_entries_are_valid(self):
        self.install()
        generated = self.home / ".local/share/applications/hakux-lr.desktop"
        result = subprocess.run(["desktop-file-validate", str(DESKTOP), str(generated)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
