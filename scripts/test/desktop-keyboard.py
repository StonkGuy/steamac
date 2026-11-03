#!/usr/bin/env python3
"""Host-side keyboard sync behavior with fake KDE commands and guest XKB files."""
import contextlib
import importlib.machinery
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[2] / "guest/layer/usr/lib/steamac/desktop-keyboard"
loader = importlib.machinery.SourceFileLoader("desktop_keyboard", str(path))
spec = importlib.util.spec_from_loader(loader.name, loader)
keyboard = importlib.util.module_from_spec(spec)
loader.exec_module(keyboard)


class DesktopKeyboardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.home = self.root / "home"
        self.home.mkdir()
        self.runtime = self.root / "runtime"
        self.bus_file = self.runtime / "nested_plasma/steamac-session-bus"
        self.bus_file.parent.mkdir(parents=True)
        self.bus_file.write_text("unix:path=/fake/plasma-bus\n")
        self.symbols = self.root / "symbols"
        self.symbols.mkdir()
        for layout in ("us", "ru", "es", "de", "fr", "ca", "gb", "br"):
            (self.symbols / layout).write_text('default xkb_symbols "basic" { };\n')
        with (self.symbols / "us").open("a") as out:
            out.write('xkb_symbols "dvorak-l" { };\nxkb_symbols "dvorak-r" { };\n'
                      '// xkb_symbols "ghost" { };\n/* xkb_symbols "phantom" { }; */\n')
        with (self.symbols / "ru").open("a") as out:
            out.write('partial alphanumeric_keys xkb_symbols "phonetic_mac" { };\n')
        with (self.symbols / "ca").open("a") as out:
            out.write('xkb_symbols "fr-legacy" { };\n')
        self.marker_path = self.home / ".config/steamac/mac-keyboard.json"
        self.config = self.desired("us")
        self.hide_variants = False
        self.live_layouts = self.rows("us", "")
        self.calls = []
        self.now = 0.0
        self.bus_live = True
        self.status_timeout = False
        self.ready_at = 0.0
        self.reload_delay = 0.4
        self.pending_reload = None
        self.set_failure = False
        self.start_patch(patch.dict(os.environ, {"HOME": str(self.home),
                                                "XDG_RUNTIME_DIR": str(self.runtime)}))
        self.start_patch(patch.object(keyboard, "XKB_SYMBOLS", self.symbols))
        self.start_patch(patch.object(keyboard.subprocess, "run", self.run_command))
        self.start_patch(patch.object(keyboard.time, "monotonic", lambda: self.now))
        self.start_patch(patch.object(keyboard.time, "sleep", self.sleep))

    def start_patch(self, patcher):
        patcher.start()
        self.addCleanup(patcher.stop)

    @staticmethod
    def desired(layouts, variants=None):
        if variants is None:
            variants = "," * layouts.count(",")
        return {"Use": "true", "LayoutList": layouts, "VariantList": variants,
                "DisplayNames": ""}

    def rows(self, layouts, variants):
        return [[layout, "", layout if self.hide_variants else layout + "(" + variant + ")"]
                for layout, variant in zip(layouts.split(","), variants.split(","))]

    def sleep(self, seconds):
        self.now += seconds

    def run_command(self, cmd, **kwargs):
        self.calls.append((list(cmd), dict(kwargs), self.now))
        if cmd[0] == "kreadconfig6":
            key = cmd[cmd.index("--key") + 1]
            return subprocess.CompletedProcess(cmd, 0, self.config.get(key, "") + "\n", "")
        if cmd[0] == "kwriteconfig6":
            key = cmd[cmd.index("--key") + 1]
            self.config[key] = cmd[-1]
            return subprocess.CompletedProcess(cmd, 0, "", "")
        self.assertEqual(cmd[0], "busctl")
        self.assertIn("--address=unix:path=/fake/plasma-bus", cmd)
        self.assertIn("--timeout=1", cmd)
        if "status" in cmd:
            self.assertLessEqual(kwargs["timeout"], 1)
            if self.status_timeout:
                raise subprocess.TimeoutExpired(cmd, kwargs["timeout"])
            return subprocess.CompletedProcess(cmd, 0 if self.bus_live else 1, "", "")
        if "getLayoutsList" in cmd:
            if self.now < self.ready_at:
                return subprocess.CompletedProcess(cmd, 1, "", "not registered")
            if self.pending_reload is not None and self.now >= self.pending_reload:
                self.live_layouts = self.rows(self.config["LayoutList"], self.config["VariantList"])
                self.pending_reload = None
            data = json.dumps({"type": "a(sss)", "data": [self.live_layouts]})
            return subprocess.CompletedProcess(cmd, 0, data, "")
        if "reloadConfig" in cmd:
            self.pending_reload = self.now + self.reload_delay
            return subprocess.CompletedProcess(cmd, 0, "", "")
        if "setLayout" in cmd:
            self.assertEqual(cmd[-2], "u")
            return subprocess.CompletedProcess(cmd, 1 if self.set_failure else 0, "", "failed")
        self.fail("unexpected command: " + repr(cmd))

    def invoke(self, index, *layouts):
        with contextlib.redirect_stderr(io.StringIO()):
            return keyboard.main(["desktop-keyboard", str(index), *layouts])

    def commands(self, name):
        return [cmd for cmd, _, _ in self.calls if name in cmd]

    def marker(self):
        return json.loads(self.marker_path.read_text())

    def test_unknown_layout_and_variant_are_filtered_and_index_recomputed(self):
        self.assertEqual(self.invoke(3, "zz", "us", "es(mac)", "ru(phonetic_mac)"), 0)
        self.assertEqual(self.config, self.desired("us,ru", ",phonetic_mac"))
        self.assertEqual(self.commands("setLayout")[-1][-1], "1")
        self.assertEqual(self.marker()["last"], self.config)

    def test_unknown_active_layout_or_variant_never_switches(self):
        for unknown in ("zz", "es(mac)"):
            with self.subTest(unknown=unknown):
                self.calls.clear()
                self.assertEqual(self.invoke(1, "us", unknown, "ru"), 0)
                self.assertEqual(self.config, self.desired("us,ru"))
                self.assertFalse(self.commands("setLayout"))

    def test_comment_declarations_are_not_variants(self):
        self.assertEqual(self.invoke(2, "us(ghost)", "us(phantom)", "us"), 0)
        self.assertEqual(self.config, self.desired("us"))
        self.assertEqual(self.commands("setLayout")[-1][-1], "0")

    def test_hyphenated_variants(self):
        self.assertEqual(self.invoke(2, "us(dvorak-l)", "us(dvorak-r)", "ca(fr-legacy)"), 0)
        self.assertEqual(self.config, self.desired("us,us,ca", "dvorak-l,dvorak-r,fr-legacy"))
        self.assertEqual(self.commands("setLayout")[-1][-1], "2")

    def test_more_than_four_keeps_active_and_original_order(self):
        self.assertEqual(self.invoke(5, "us", "ru", "es", "de", "fr", "ca"), 0)
        self.assertEqual(self.config, self.desired("us,ru,es,ca"))
        self.assertEqual(self.commands("setLayout")[-1][-1], "3")

    def test_more_than_four_with_early_active_keeps_first_four(self):
        self.assertEqual(self.invoke(1, "us", "ru", "es", "de", "fr"), 0)
        self.assertEqual(self.config, self.desired("us,ru,es,de"))
        self.assertEqual(self.commands("setLayout")[-1][-1], "1")

    def test_first_sync_owns_existing_config_and_clears_display_names(self):
        self.config = {"Use": "false", "LayoutList": "de,fr", "VariantList": ",",
                       "DisplayNames": "Deutsch,Francais"}
        self.assertEqual(self.invoke(0, "us", "ru"), 0)
        self.assertEqual(self.config, self.desired("us,ru"))
        self.assertEqual(self.marker(), {"last": self.config})
        self.assertEqual(self.commands("kwriteconfig6")[-1][-1], "")
        self.assertEqual(self.marker_path.stat().st_mode & 0o777, 0o600)

    def test_matching_first_sync_still_establishes_ownership(self):
        self.assertEqual(self.invoke(0, "us"), 0)
        self.assertFalse(self.commands("kwriteconfig6"))
        self.assertEqual(self.marker(), {"last": self.desired("us")})
        self.config["LayoutList"] = "de"
        self.assertEqual(self.invoke(0, "us"), 0)
        self.assertTrue(self.marker()["user"])
        self.assertEqual(self.config["LayoutList"], "de")

    def test_observed_edit_of_each_owned_key_is_permanent(self):
        for key, value in (("Use", "false"), ("LayoutList", "de"),
                           ("VariantList", "dvorak-l"), ("DisplayNames", "Mine")):
            with self.subTest(key=key):
                self.marker_path.unlink(missing_ok=True)
                self.config = self.desired("us")
                self.assertEqual(self.invoke(0, "us"), 0)
                self.config[key] = value
                edited = dict(self.config)
                self.calls.clear()
                self.assertEqual(self.invoke(0, "ru"), 0)
                self.assertEqual(self.config, edited)
                self.assertTrue(self.marker()["user"])
                self.assertFalse(self.commands("kwriteconfig6"))
                self.assertFalse(self.commands("getLayoutsList"))
                self.assertFalse(self.commands("setLayout"))
                # Restoring our old value must not undo the permanent opt-out.
                self.config = self.desired("us")
                self.calls.clear()
                self.assertEqual(self.invoke(0, "ru"), 0)
                self.assertEqual(self.config, self.desired("us"))
                self.assertFalse(self.commands("kreadconfig6"))
                self.assertFalse(self.commands("reloadConfig"))

    def test_matching_file_but_different_kwin_codes_reloads(self):
        self.live_layouts = self.rows("de", "")
        self.assertEqual(self.invoke(0, "us"), 0)
        self.assertFalse(self.commands("kwriteconfig6"))
        self.assertEqual(len(self.commands("reloadConfig")), 1)
        self.assertEqual(self.live_layouts, self.rows("us", ""))
        self.assertTrue(self.commands("setLayout"))

    def test_variant_reload_waits_for_asynchronous_kwin_change(self):
        self.reload_delay = 0.8
        self.assertEqual(self.invoke(0, "us(dvorak-l)"), 0)
        switch_time = next(at for cmd, _, at in self.calls if "setLayout" in cmd)
        self.assertGreaterEqual(switch_time, self.reload_delay)
        self.assertEqual(self.live_layouts, self.rows("us", "dvorak-l"))

    def test_variant_reload_with_unchanged_names_still_settles(self):
        self.hide_variants = True
        self.live_layouts = self.rows("us", "")
        self.assertEqual(self.invoke(0, "us(dvorak-r)"), 0)
        reload_time = next(at for cmd, _, at in self.calls if "reloadConfig" in cmd)
        switch_time = next(at for cmd, _, at in self.calls if "setLayout" in cmd)
        self.assertGreaterEqual(switch_time - reload_time, 2)

    def test_live_bus_waits_for_late_keyboard_registration_then_reloads(self):
        self.ready_at = 9.0
        self.assertEqual(self.invoke(1, "us", "ru"), 0)
        self.assertEqual(len(self.commands("reloadConfig")), 1)
        reload_time = next(at for cmd, _, at in self.calls if "reloadConfig" in cmd)
        self.assertGreaterEqual(reload_time, self.ready_at)
        self.assertEqual(self.commands("setLayout")[-1][-1], "1")

    def test_live_bus_without_keyboard_service_has_bounded_wait(self):
        self.ready_at = 100.0
        self.assertEqual(self.invoke(0, "us"), 1)
        self.assertGreaterEqual(self.now, 15)
        self.assertLess(self.now, 15.3)
        self.assertFalse(self.commands("reloadConfig"))
        self.assertFalse(self.commands("setLayout"))

    def test_missing_desktop_exits_before_commands_or_marker_access(self):
        self.bus_file.unlink()
        self.assertEqual(self.invoke(0, "us"), 0)
        self.assertFalse(self.calls)
        self.assertFalse(self.marker_path.exists())
        self.assertEqual(self.now, 0)

    def test_stale_or_timed_out_desktop_exits_before_config_access(self):
        for timeout in (False, True):
            with self.subTest(timeout=timeout):
                self.calls.clear()
                self.bus_live = False
                self.status_timeout = timeout
                self.assertEqual(self.invoke(0, "us"), 0)
                self.assertEqual(len(self.calls), 1)
                self.assertTrue(self.commands("status"))
                self.assertFalse(self.marker_path.exists())
                self.assertEqual(self.now, 0)

    def test_all_unavailable_leaves_config_and_marker_untouched(self):
        self.assertEqual(self.invoke(0, "zz", "es(mac)"), 0)
        self.assertFalse(self.commands("kreadconfig6"))
        self.assertFalse(self.commands("setLayout"))
        self.assertFalse(self.marker_path.exists())

    def test_invalid_syntax_and_index_are_rejected(self):
        for index, layouts in ((0, ["../us"]), (0, ["us(bad/variant)"]),
                               (0, ["us\n"]), (-1, ["us"]), (1, ["us"])):
            with self.subTest(index=index, layouts=layouts):
                self.assertEqual(self.invoke(index, *layouts), 2)
        self.assertFalse(self.calls)

    def test_set_layout_failure_is_reported(self):
        self.set_failure = True
        self.assertEqual(self.invoke(0, "us"), 1)
        self.assertEqual(len(self.commands("setLayout")), 1)


if __name__ == "__main__":
    unittest.main()
