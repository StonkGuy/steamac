#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
HOOK = ROOT / "guest/layer/usr/lib/steamac/steam-fex-eac"
RUN = ROOT / "guest/layer/usr/lib/steamac/eac-launch"
FILES = ("FEXCompatTool", "VERSIONS.txt", "ConfigTemplate.json")
BINS = ("FEX", "FEXBash", "FEXGetConfig", "FEXOfflineCompiler", "FEXServer", "FEXServerManager", "FEXpidof")


class GameSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.steam = self.home / ".local/share/Steam"
        self.tool = self.steam / "steamapps/common/FEX-Emu"
        self.stage = self.home / "fex-eac-work/stage"
        self.cmdline = self.home / "cmdline"
        self.cmdline.write_text("steamac.fex_eac=1")
        self.env = dict(os.environ, HOME=str(self.home), STEAMAC_CMDLINE=str(self.cmdline),
                        STEAMAC_FEX_DIR=str(ROOT / "fex-eac"), XDG_STATE_HOME=str(self.home / "state"))
        self.bin = self.home / "bin"
        self.bin.mkdir()
        (self.bin / "pgrep").write_text("#!/bin/sh\nexit 1\n")
        (self.bin / "systemctl").write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$HOME/service-calls"\n')
        for file in self.bin.iterdir():
            file.chmod(0o755)
        self.env["PATH"] = str(self.bin) + os.pathsep + os.environ["PATH"]
        self.source = next(line.split("=", 1)[1] for line in (ROOT / "fex-eac/build-fex.sh").read_text().splitlines()
                           if line.startswith("FEX_EAC_SHA="))
        self.seed(self.tool, "FEX-valve")
        self.seed(self.stage, "FEX-patched")
        (self.stage / ".steamac-fex-source").write_text(self.source + "\n")

    def seed(self, root, value):
        (root / "usr/bin").mkdir(parents=True)
        for name in BINS:
            (root / "usr/bin" / name).write_text(value + " " + name)
        for name in FILES:
            (root / name).write_text("FEX describe\t" + value + "\n" if name == "VERSIONS.txt" else value)

    def hook(self, *args, expected=0):
        result = subprocess.run(["python3", str(HOOK), *args], env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_disabled_does_not_change_steam_or_schedule_build(self):
        self.cmdline.write_text("steamac.fex_eac=0")
        before = (self.tool / "usr/bin/FEX").read_bytes()
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_bytes(), before)
        self.assertFalse((self.home / "service-calls").exists())

    def test_missing_or_unverified_stage_requests_background_build(self):
        (self.stage / ".steamac-fex-source").unlink()
        output = self.hook()
        self.assertIn("--user start --no-block steamac-fex-build.service", (self.home / "service-calls").read_text())
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-valve FEX")
        self.assertIn("Preparing", output)

    def test_staged_runtime_installs_and_repairs_steam_update(self):
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-patched FEX")
        before = (self.tool / "usr/bin/FEX").stat().st_mtime_ns
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").stat().st_mtime_ns, before)
        for name in BINS:
            (self.tool / "usr/bin" / name).write_text("FEX-valve-update " + name)
        (self.tool / "VERSIONS.txt").write_text("FEX describe\tFEX-valve-update\n")
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-patched FEX")

    def test_disabled_restores_only_our_installed_runtime(self):
        self.hook()
        self.cmdline.write_text("steamac.fex_eac=0")
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-valve FEX")
        self.cmdline.write_text("steamac.fex_eac=1")
        self.hook()
        (self.tool / "usr/bin/FEX").write_text("foreign-runtime")
        self.cmdline.write_text("steamac.fex_eac=0")
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "foreign-runtime")
        self.assertFalse((self.home / "state/steamac/fex-eac/installed.json").exists())

    def test_disabled_keeps_a_new_steam_runtime_without_repeated_errors(self):
        self.hook()
        for name in BINS:
            (self.tool / "usr/bin" / name).write_text("FEX-valve-update " + name)
        self.cmdline.write_text("steamac.fex_eac=0")
        self.hook()
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-valve-update FEX")
        self.assertFalse((self.home / "state/steamac/fex-eac/installed.json").exists())

    def test_restore_keeps_the_last_steam_update(self):
        self.hook()
        for name in BINS:
            (self.tool / "usr/bin" / name).write_text("FEX-valve-update " + name)
        (self.tool / "VERSIONS.txt").write_text("FEX describe\tFEX-valve-update\n")
        self.hook()
        self.cmdline.write_text("steamac.fex_eac=0")
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-valve-update FEX")

    def test_rebuilt_stage_does_not_replace_the_original_backup(self):
        self.hook()
        for name in BINS:
            (self.stage / "usr/bin" / name).write_text("FEX-rebuilt " + name)
        (self.stage / "VERSIONS.txt").write_text("FEX describe\tFEX-rebuilt\n")
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-rebuilt FEX")
        self.cmdline.write_text("steamac.fex_eac=0")
        self.hook()
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-valve FEX")

    def test_status_detects_replaced_runtime(self):
        self.hook()
        (self.tool / "usr/bin/FEX").write_text("replaced")
        output = self.hook("status")
        self.assertIn("changed", output)
        self.assertNotIn("Runtime installed", output)

    def test_running_steam_prevents_runtime_and_config_changes(self):
        (self.bin / "pgrep").write_text('#!/bin/sh\n[ "$2" = steam ]\n')
        self.hook(expected=1)
        self.assertEqual((self.tool / "usr/bin/FEX").read_text(), "FEX-valve FEX")

    def test_vrchat_options_are_scoped_and_user_options_survive(self):
        (self.steam / "steamapps/appmanifest_438100.acf").write_text('"AppState" { "appid" "438100" }')
        (self.steam / "steamapps/common/Proton - Experimental").mkdir()
        (self.steam / "steamapps/common/Proton EasyAntiCheat Runtime").mkdir()
        config = self.steam / "config/config.vdf"
        config.parent.mkdir()
        config.write_text('"InstallConfigStore" { "Software" { "Valve" { "Steam" { "Other" "keep" } } } }\n')
        account = self.steam / "userdata/123/config/localconfig.vdf"
        account.parent.mkdir(parents=True)
        original = '"UserLocalConfigStore" { "Other" { "438100" { "LaunchOptions" "keep" } } "Software" { "Valve" { "Steam" { "apps" { "438100" { "name" "VRChat" } "9" { "LaunchOptions" "other" } } } } } }\n'
        account.write_text(original)
        self.hook()
        changed = account.read_text()
        self.assertIn('/usr/lib/steamac/eac-launch', changed)
        self.assertIn('"LaunchOptions" "keep"', changed)
        self.assertIn('"LaunchOptions" "other"', changed)
        self.assertIn('proton_experimental', config.read_text())
        self.assertEqual(Path(str(account) + ".steamac-fex-backup").read_text(), original)
        account.write_text(changed.replace('"/usr/lib/steamac/eac-launch 438100 %command%"', '"user-options"'))
        user = account.read_bytes()
        self.hook()
        self.assertEqual(account.read_bytes(), user)

    def test_first_setup_selects_proton_before_fex_is_downloaded(self):
        (self.tool / "VERSIONS.txt").unlink()
        (self.stage / ".steamac-fex-source").unlink()
        (self.steam / "steamapps/appmanifest_438100.acf").write_text('"AppState" { "appid" "438100" }')
        for name in ("Proton - Experimental", "Proton EasyAntiCheat Runtime"):
            (self.steam / "steamapps/common" / name).mkdir()
        config = self.steam / "config/config.vdf"
        config.parent.mkdir()
        config.write_text('"InstallConfigStore" { "Software" { "Valve" { "Steam" { } } } }')
        self.hook()
        self.assertIn("proton_experimental", config.read_text())

    def test_wrapper_preserves_command_arguments_and_exit_status(self):
        runtime = self.steam / "steamapps/common/Proton EasyAntiCheat Runtime"
        runtime.mkdir()
        command = ["python3", "-c", "import json,os,sys; print(json.dumps([sys.argv[1:],os.environ.get('EAC_LAUNCHERDIR'),os.environ.get('PROTON_EAC_RUNTIME')])); sys.exit(7)", "two words", "$(false)"]
        result = subprocess.run(["bash", str(RUN), "438100", *command], env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 7, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data[0], ["two words", "$(false)"])
        self.assertEqual(data[1], str(self.steam / "steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat"))
        self.assertEqual(data[2], str(runtime))

    def test_wrapper_disabled_leaves_environment_and_command_intact(self):
        self.cmdline.write_text("steamac.fex_eac=0")
        result = subprocess.run(["bash", str(RUN), "438100", "python3", "-c", "import os; print(os.environ.get('EAC_LAUNCHERDIR', 'unset'))"],
                                env=self.env, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "unset")


if __name__ == "__main__":
    unittest.main()
