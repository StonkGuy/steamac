#!/usr/bin/env python3
"""Host-side ownership/device checks with real files and a stub Vulkan probe."""
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

path = Path(__file__).resolve().parents[2] / "guest/layer/usr/lib/steamac/steam-title-defaults"
loader = importlib.machinery.SourceFileLoader("steam_title_defaults", str(path))
spec = importlib.util.spec_from_loader(loader.name, loader)
titles = importlib.util.module_from_spec(spec)
sys.dont_write_bytecode = True
loader.exec_module(titles)
KEY = (*titles.VDF_PATH, "2054970", "LaunchOptions")
WORDS = " ".join(titles.TITLES["2054970"][0])


class TitleDefaultsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="steamac-title-test-")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.config = self.home / ".local/share/Steam/userdata/123/config/localconfig.vdf"
        self.marker = self.home / ".config/steamac/title-defaults.json"
        self.bin = self.home / "bin"
        self.bin.mkdir()
        timeout = self.bin / "timeout"
        timeout.write_text('#!/bin/sh\nshift 3\nexec "$@"\n')
        timeout.chmod(0o755)
        probe = self.bin / "vulkaninfo"
        probe.write_text('#!' + sys.executable + '\nfrom pathlib import Path\nimport os, sys\nhome = Path(os.environ["HOME"])\nwith (home / "calls").open("a") as out: out.write("probe\\n")\nmode = (home / "mode").read_text()\nif mode == "fail": sys.exit(1)\nprint("GPU0:\\n driverID = DRIVER_ID_MESA_LLVMPIPE\\n shaderBufferInt64Atomics = true\\n shaderImageInt64Atomics = true")\nprint("GPU1:\\n driverID = DRIVER_ID_MESA_VENUS\\n shaderBufferInt64Atomics = " + mode + "\\n shaderImageInt64Atomics = " + mode)\n')
        probe.chmod(0o755)
        (self.home / "mode").write_text("true")
        self.env = dict(os.environ, HOME=str(self.home), PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        self.set_options(None)

    def set_options(self, value):
        self.config.parent.mkdir(parents=True, exist_ok=True)
        text = '"UserLocalConfigStore" { "Unrelated" "keep" } // retained\n'
        if value is not None:
            text = titles.vdf_set(text, KEY, value)
        self.config.write_text(text)

    def options(self):
        values, _ = titles.vdf_entries(self.config.read_text())
        stored = values.get(tuple(part.lower() for part in KEY))
        return stored[0] if stored else None

    def state(self):
        return json.loads(self.marker.read_text())["accounts"]["123"]["2054970"]

    def run_script(self):
        result = subprocess.run([sys.executable, str(path)], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"Unrelated" "keep"', self.config.read_text())
        self.assertTrue(self.config.read_text().endswith('// retained\n'))
        return result.stderr

    def test_apply_idempotence_and_revert(self):
        for previous in (None, "", "FOO=bar %command% --custom"):
            with self.subTest(previous=previous):
                self.marker.unlink(missing_ok=True)
                self.set_options(previous)
                (self.home / "mode").write_text("true")
                self.run_script()
                expected = WORDS + " " + (previous or "%command%")
                self.assertEqual(self.options(), expected)
                self.assertEqual(self.state()["previous"], previous)
                config, marker = self.config.read_bytes(), self.marker.read_bytes()
                self.run_script()
                self.assertEqual(self.config.read_bytes(), config)
                self.assertEqual(self.marker.read_bytes(), marker)
                (self.home / "mode").write_text("false")
                self.run_script()
                self.assertEqual(self.options(), previous)
                self.assertEqual(self.state()["last"], previous)
                self.assertFalse(self.state()["active"])
                self.run_script()
                self.assertEqual(self.options(), previous)
                (self.home / "mode").write_text("true")
                self.run_script()
                self.assertEqual(self.options(), expected)

    def test_user_edit_wins_without_probe(self):
        self.run_script()
        ours = self.options()
        self.set_options("USER=1 %command%")
        calls = (self.home / "calls").read_bytes()
        self.run_script()
        self.assertEqual(self.options(), "USER=1 %command%")
        self.assertTrue(self.state()["user"])
        self.set_options(ours)
        self.run_script()
        self.assertEqual((self.home / "calls").read_bytes(), calls)

    def test_hand_added_words_become_owned(self):
        self.set_options(WORDS + " %command% --hand")
        self.run_script()
        self.assertEqual(self.state()["last"], self.options())
        (self.home / "mode").write_text("false")
        self.run_script()
        self.assertEqual(self.options(), "%command% --hand")
        (self.home / "mode").write_text("true")
        self.run_script()
        self.set_options("%command% --hand")
        self.run_script()
        self.assertTrue(self.state()["user"])

    def test_corrupt_markers_recover(self):
        for broken in ("{", "[]", '{"accounts": []}', '{"accounts":{"123":{"2054970":false}}}'):
            with self.subTest(marker=broken):
                self.set_options(None)
                self.marker.parent.mkdir(parents=True, exist_ok=True)
                self.marker.write_text(broken)
                self.assertIn("ignoring corrupt marker", self.run_script())
                self.assertEqual(self.options(), WORDS + " %command%")
                self.assertTrue(self.state()["active"])

    def test_false_venus_is_not_cpu_features(self):
        (self.home / "mode").write_text("false")
        self.run_script()
        self.assertIsNone(self.options())
        self.assertNotIn("last", self.state())

    def test_raw_arguments_do_not_probe(self):
        self.set_options("--raw-arguments")
        self.run_script()
        self.assertEqual(self.options(), "--raw-arguments")
        self.assertFalse((self.home / "calls").exists())

    def test_failed_probe_does_not_apply_or_revert(self):
        (self.home / "mode").write_text("fail")
        self.run_script()
        self.assertIsNone(self.options())
        (self.home / "mode").write_text("true")
        self.run_script()
        ours = self.options()
        (self.home / "mode").write_text("fail")
        self.run_script()
        self.assertEqual(self.options(), ours)
        (self.bin / "vulkaninfo").unlink()
        self.run_script()
        self.assertEqual(self.options(), ours)

    def test_failed_config_write_does_not_claim_ownership(self):
        # Exercise main in-process only for the write failure; the probe/file scenarios
        # above run the actual script with an isolated HOME and PATH.
        from unittest.mock import patch
        real_write = titles.atomic_write

        def fail_config(config, text):
            if config == self.config:
                raise OSError("stub config write failure")
            return real_write(config, text)

        with patch.dict(os.environ, self.env), patch.object(titles, "_features", None), patch.object(titles, "atomic_write", fail_config):
            titles.main()
        self.assertIsNone(self.options())
        self.assertNotIn("last", self.state())


if __name__ == "__main__":
    unittest.main()
