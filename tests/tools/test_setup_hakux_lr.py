# SPDX-License-Identifier: GPL-2.0-or-later
"""Installer and launcher regressions using local cores and fake external tools."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest


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
        for name in ("sed", "uname", "id", "grep", "ldd", "flatpak", "curl",
                     "sudo", "pacman", "pgrep", "steamos-add-to-steam"):
            tool = self.bindir / name
            tool.write_text(FAKE_TOOL.replace("PYTHON", sys.executable, 1))
            tool.chmod(0o755)
        # Exclude host package managers and RetroArch from auto-detection.
        for name in ("sh", "tr", "head", "cut", "ls", "sort", "readlink", "stat",
                     "getent", "cat", "mkdir", "mktemp", "rm", "cp", "mv", "find",
                     "tail", "ln", "chmod", "dirname", "tar", "gzip", "sha256sum"):
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

    def test_steamos_uses_flatpak_without_root(self):
        # Force the missing-KVM path even when the test host has /dev/kvm.
        if Path("/dev/kvm").exists():
            self.skipTest("requires a host without /dev/kvm")
        result = self.install(TEST_ARCH="x86_64")
        self.assertFalse(self.calls("sudo"))
        self.assertFalse(self.calls("pacman"))
        self.assertIn("sudo modprobe kvm_amd", result.stderr)
        self.assertIn("RA_KIND='flatpak'", (self.prefix / "hakux-lr.env").read_text())

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
        self.assertIn("RA_KIND='system'", (self.prefix / "hakux-lr.env").read_text())
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
        result = subprocess.run([str(self.prefix / "start-hakux-lr.sh"), "halo"],
                                env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(str(games / "Halo.iso"), self.calls("flatpak")[-1])
        self.assertIn(str(self.prefix / "cores/hakux-lr_libretro.so"), self.calls("flatpak")[-1])

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
