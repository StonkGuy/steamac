#!/usr/bin/env python3
"""Bounded first-sync wait and exact applied-set digest using isolated guest paths."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "guest/layer/usr/lib/steamac/steam-profiles-wait"


class ProfilesWaitTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="steamac-profile-wait-")
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name)
        self.runtime = self.home / "runtime/steamac"
        self.runtime.mkdir(parents=True)
        self.profiles = self.home / ".config/steamac/game-profiles.json"
        self.profiles.parent.mkdir(parents=True)
        self.env = dict(os.environ, HOME=str(self.home), XDG_RUNTIME_DIR=str(self.runtime.parent))

    def run_script(self):
        start = time.monotonic()
        result = subprocess.run([sys.executable, str(SCRIPT)], env=self.env, capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        return time.monotonic() - start

    def assert_applied(self, data):
        self.assertEqual((self.runtime / "game-profiles.json").read_bytes(), data)
        self.assertEqual((self.runtime / "profiles-applied.sha256").read_text(), hashlib.sha256(data).hexdigest() + "\n")

    def test_timeout_uses_persisted_file(self):
        data = b'{ "v": 1, "games": {"570": {"gfx": "dxmt"}} }\n'
        self.profiles.write_bytes(data)
        elapsed = self.run_script()
        self.assertGreaterEqual(elapsed, 4.9)
        self.assertLess(elapsed, 6.5)
        self.assert_applied(data)
        self.assertFalse((self.runtime / "profiles-synced").exists())

    def test_synced_set_is_immediate_and_each_start_refreshes_digest(self):
        (self.runtime / "profiles-synced").touch()
        for data in [b'{"v":1,"games":{}}', b'{"games":{"570":{"env":"PROTON_LOG=1"}},"v":1}']:
            self.profiles.write_bytes(data)
            self.assertLess(self.run_script(), 2)
            self.assert_applied(data)
        # A host edit cannot retroactively change what this Steam start applied.
        self.profiles.write_bytes(b'{"v":1,"games":{}}')
        self.assert_applied(data)

    def test_no_persisted_profiles_snapshots_empty_set(self):
        (self.runtime / "profiles-synced").touch()
        self.run_script()
        self.assert_applied(b"")


if __name__ == "__main__":
    unittest.main()
