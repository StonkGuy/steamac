#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""guest/layer/usr/lib/steamac/ytdlp/ytdlp_server.py (steamac.ytdlp=native):
  split     Windows command-line splitting (CommandLineToArgvW rules, Microsoft's documented examples)
  map       VRChat's yt-dlp.exe arguments -> native yt-dlp arguments; unsafe options dropped; paths mapped
  protocol  the stub protocol end to end against a fake native yt-dlp (stdout, stderr, exit code,
            fallback answer when no native yt-dlp is installed)
usage: scripts/test/ytdlp-server.py"""
import importlib.util
import os
import socket
import sys
import tempfile
import threading
import unittest

sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "../../guest/layer/usr/lib/steamac/ytdlp/ytdlp_server.py")
spec = importlib.util.spec_from_file_location("ytdlp_server", SRC)
ys = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ys)

# VRChat's argument shapes: AVPro and the Unity player (the latter carries [protocol^=http])
AVPRO = ('--no-check-certificate --no-cache-dir --rm-cache-dir '
         '-f "mp4[height<=?1080][height>=?64][width>=?64]/best[height<=?1080][height>=?64][width>=?64]" '
         '--get-url "https://www.youtube.com/watch?v=aaaaaaaaaaa"')
UNITY = ('--no-check-certificate --no-cache-dir --rm-cache-dir -f "(mp4/best)[height<=?720][protocol^=http]" '
         '--get-url "https://www.youtube.com/watch?v=aaaaaaaaaaa"')


class Split(unittest.TestCase):
    def test_microsoft_examples(self):
        # learn.microsoft.com "Parsing C++ command-line arguments"
        for line, want in [('"abc" d e', ["abc", "d", "e"]),
                           (r'a\\\b d"e f"g h', [r"a\\\b", "de fg", "h"]),
                           (r'a\\\"b c d', [r'a\"b', "c", "d"]),
                           (r'a\\\\"b c" d e', [r"a\\b c", "d", "e"]),
                           ('a"b"" c d', ['ab" c d'])]:
            self.assertEqual(ys.split_cmdline(line), want, line)

    def test_empty_and_spaces(self):
        self.assertEqual(ys.split_cmdline(""), [])
        self.assertEqual(ys.split_cmdline('  \t ""  x '), ["", "x"])

    def test_vrchat(self):
        a = ys.split_cmdline(AVPRO)
        self.assertEqual(a[3:5], ["-f", "mp4[height<=?1080][height>=?64][width>=?64]/best[height<=?1080][height>=?64][width>=?64]"])
        self.assertEqual(a[-1], "https://www.youtube.com/watch?v=aaaaaaaaaaa")


class Map(unittest.TestCase):
    def m(self, line, drive_c="/pfx/drive_c"):
        return ys.map_args(ys.split_cmdline(line), drive_c)

    def test_vrchat_passes_unchanged(self):
        for line in (AVPRO, UNITY):
            out, notes, upd = self.m(line)
            self.assertEqual(out, ["--ignore-config"] + ys.split_cmdline(line))
            self.assertEqual(notes, [])
            self.assertFalse(upd)

    def test_inline_values(self):
        out, _, _ = self.m('--impersonate=safari --format=best https://x.example/v')
        self.assertEqual(out, ["--ignore-config", "--impersonate", "safari", "--format", "best", "https://x.example/v"])

    def test_unsafe_dropped_with_value(self):
        out, notes, _ = self.m('--exec "calc.exe {}" -o "C:\\x\\%(id)s" --config-location Z:\\tmp\\c '
                               '--js-runtimes deno:C:\\deno.exe --ffmpeg-location=C:\\ff -g https://a.example/v')
        self.assertEqual(out, ["--ignore-config", "-g", "https://a.example/v"])
        self.assertEqual(len(notes), 5)

    def test_unknown_option_and_positional(self):
        out, notes, _ = self.m("--frobnicate notaurl https://a.example/v")
        self.assertEqual(out, ["--ignore-config", "https://a.example/v"])
        self.assertEqual(len(notes), 2)

    def test_paths(self):
        out, _, _ = self.m(r'--cookies "C:\users\steamuser\c.txt" https://a.example/v')
        self.assertEqual(out[1:3], ["--cookies", "/pfx/drive_c/users/steamuser/c.txt"])
        self.assertEqual(ys.unix_path("Z:\\home\\deck\\c.txt", None), "/home/deck/c.txt")
        self.assertEqual(ys.unix_path("D:\\c.txt", "/p"), "D:\\c.txt")

    def test_update(self):
        out, _, upd = self.m("-U")
        self.assertEqual((out, upd), ([], True))

    def test_missing_value(self):
        out, notes, _ = self.m("https://a.example/v -f")
        self.assertEqual(out, ["--ignore-config", "https://a.example/v"])
        self.assertEqual(len(notes), 1)


def ask(port, line):
    """What the stub does: send the command line, read the frames."""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
        return _ask(s, line)


def _ask(s, line):
    b = line.encode()
    s.sendall(b"STEAMAC-YTDLP 1 %d\n" % len(b) + b)
    f = s.makefile("rb")  # closed with the socket
    got = {"O": b"", "E": b""}
    while True:
        h = f.readline().decode().strip()
        if h == "F":
            return "fallback", got
        if h.startswith("X"):
            return int(h[1:]), got
        got[h[0]] += f.read(int(h[1:]))


class Protocol(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        ys.DATA = cls.tmp.name
        ys.Handler.drive_c = "/pfx/drive_c"
        cls.srv = ys.Server(("127.0.0.1", 0), ys.Handler)
        cls.port = cls.srv.server_address[1]
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()
        cls.tmp.cleanup()

    def install_fake(self, body):
        p = os.path.join(self.tmp.name, "yt-dlp")
        with open(p, "w") as f:
            f.write(body)

    def test_a_fallback_without_native(self):
        rc, _ = ask(self.port, AVPRO)
        self.assertEqual(rc, "fallback")

    def test_b_output_and_code(self):
        # the fake prints its argv as stdout, a note on stderr, exits 3
        self.install_fake("import sys\nprint('\\n'.join(sys.argv[1:]))\nsys.stderr.write('warn\\n')\nsys.exit(3)\n")
        rc, got = ask(self.port, UNITY + " --exec rm")
        self.assertEqual(rc, 3)
        self.assertEqual(got["O"].decode().split("\n")[:-1], ["--ignore-config"] + ys.split_cmdline(UNITY))
        self.assertIn(b"--exec: not passed", got["E"])
        self.assertTrue(got["E"].endswith(b"warn\n"))

    def test_c_large_output(self):
        self.install_fake("import sys\nsys.stdout.write('x' * 300000)\n")
        rc, got = ask(self.port, AVPRO)
        self.assertEqual((rc, len(got["O"])), (0, 300000))

    def test_d_bad_header_ignored(self):
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as s:
            s.sendall(b"GET / HTTP/1.0\r\n\r\n")
            self.assertEqual(s.recv(10), b"")


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
