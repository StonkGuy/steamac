#!/usr/bin/env python3
"""Host-side checks of steam-idle-screensaver's config.vdf edit and once-per-install marker."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import unittest

# Loading the hook must not leave a __pycache__ in guest/layer (it would ship in the layer).
sys.dont_write_bytecode = True

path = Path(__file__).resolve().parents[2] / "guest/layer/usr/lib/steamac/steam-idle-screensaver"
loader = importlib.machinery.SourceFileLoader("steam_idle_screensaver", str(path))
spec = importlib.util.spec_from_loader(loader.name, loader)
idle = importlib.util.module_from_spec(spec)
loader.exec_module(idle)
SYSTEM = ("installconfigstore", "software", "valve", "steam", "system")
AC = (*SYSTEM, "idlescreensaveracseconds")
BATTERY = (*SYSTEM, "idlescreensaverbatteryseconds")
# Layout as Steam writes it (tabs, one key per line).
STEAM = ('"InstallConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
         '\t\t\t\t"AutoUpdateWindowEnabled"\t\t"0"\n\t\t\t\t"ShaderCacheManager"\n\t\t\t\t{\n'
         '\t\t\t\t\t"HasCurrentBucket"\t\t"1"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n'
         '\t"SDL_GamepadBind"\t\t"keep"\n}\n')


class IdleScreensaverTests(unittest.TestCase):
    def test_fresh_install_creates_steam_layout(self):
        text, added = idle.screensaver_off(idle.EMPTY)
        self.assertEqual(added, list(idle.KEYS))
        self.assertEqual(text, '"InstallConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
                               '\t\t\t\t"System"\n\t\t\t\t{\n\t\t\t\t\t"IdleScreensaverACSeconds"\t\t"0"\n'
                               '\t\t\t\t\t"IdleScreensaverBatterySeconds"\t\t"0"\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n')

    def test_existing_config_keeps_every_other_byte(self):
        text, added = idle.screensaver_off(STEAM)
        self.assertEqual(added, list(idle.KEYS))
        values, _ = idle.vdf_entries(text)
        self.assertEqual((values[AC][0], values[BATTERY][0]), ("0", "0"))
        block = ('\t\t\t\t"System"\n\t\t\t\t{\n\t\t\t\t\t"IdleScreensaverACSeconds"\t\t"0"\n'
                 '\t\t\t\t\t"IdleScreensaverBatterySeconds"\t\t"0"\n\t\t\t\t}\n')
        self.assertIn(block, text)
        self.assertEqual(text.replace(block, "", 1), STEAM)

    def test_user_value_stays(self):
        user = STEAM.replace('\t\t\t\t"ShaderCacheManager"', '\t\t\t\t"System"\n\t\t\t\t{\n'
                             '\t\t\t\t\t"IdleScreensaverACSeconds"\t\t"300"\n\t\t\t\t}\n\t\t\t\t"ShaderCacheManager"')
        text, added = idle.screensaver_off(user)
        self.assertEqual(added, ["IdleScreensaverBatterySeconds"])
        values, _ = idle.vdf_entries(text)
        self.assertEqual((values[AC][0], values[BATTERY][0]), ("300", "0"))
        both, added = idle.screensaver_off(text)
        self.assertEqual((both, added), (text, []))

    def test_single_line_and_malformed(self):
        text, _ = idle.screensaver_off('"InstallConfigStore" { "Software" { } } // retained\n')
        values, _ = idle.vdf_entries(text)
        self.assertEqual((values[AC][0], values[BATTERY][0]), ("0", "0"))
        self.assertTrue(text.endswith('} // retained\n'))
        for bad in ['"InstallConfigStore" {', '}', '"UserLocalConfigStore" { }', '"a" { "b" "1" "b" "2" }']:
            with self.assertRaises(ValueError):
                idle.screensaver_off(bad)

    def test_once_per_install(self):
        with tempfile.TemporaryDirectory() as home:
            old = os.environ.get("HOME")
            os.environ["HOME"] = home
            try:
                config = Path(home) / ".local/share/Steam/config/config.vdf"
                idle.main()
                values, _ = idle.vdf_entries(config.read_text())
                self.assertEqual((values[AC][0], values[BATTERY][0]), ("0", "0"))
                self.assertEqual(config.stat().st_mode & 0o777, 0o644)
                # A screensaver chosen later in Steam (here: keys removed) is not overridden.
                config.write_text(STEAM)
                idle.main()
                self.assertEqual(config.read_text(), STEAM)
                # A Steam reinstall (new config dir) gets the default again.
                (config.parent / "steamac-idle-screensaver").unlink()
                idle.main()
                self.assertIn('"IdleScreensaverACSeconds"\t\t"0"', config.read_text())
            finally:
                if old is None:
                    del os.environ["HOME"]
                else:
                    os.environ["HOME"] = old


if __name__ == "__main__":
    unittest.main()
