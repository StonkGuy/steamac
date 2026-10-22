#!/usr/bin/python3
"""steamac: native yt-dlp for VRChat's video players (steamac.ytdlp=native).

VRChat starts Tools/yt-dlp.exe in its Proton prefix to resolve a video URL and waits for it. Under Proton
on this stack that is a PyInstaller Windows build running in Wine under FEX: 12-52 s per URL, and the
game stalls while it waits. With the option on, steam-ytdlp puts a small Windows stub in its place
(guest/ytdlp-stub). The stub sends its command line here (127.0.0.1 only) and copies back what a native
arm64 yt-dlp (the platform-independent zipapp under the guest's own python3) prints.

Protocol (one request per connection):
  stub -> server  b"STEAMAC-YTDLP 1 <n>\\n" + <n> bytes: the stub's arguments as UTF-8, Windows
                  command-line syntax (everything after argv[0])
  server -> stub  frames b"O<n>\\n"+data (stdout), b"E<n>\\n"+data (stderr), then b"X<code>\\n" (exit code)
                  or b"F\\n" alone: no native yt-dlp yet, the stub runs the original yt-dlp.exe instead.

Only the options a video player needs pass (ALLOWED_*); anything that writes files or runs commands
(--exec, -o, --config-location, --plugin-dirs, -U, ...) is dropped, so the port cannot be used to run
arbitrary commands through yt-dlp. Windows paths in the few path options are mapped to Unix paths.
"""
import os
import socketserver
import subprocess
import sys
import threading
import time
import hashlib
import urllib.request

PORT = 47811
DATA = os.environ.get("STEAMAC_YTDLP_DIR") or os.path.join(
    os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "steamac", "ytdlp")
RELEASE = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/"
TIMEOUT = 90          # VRChat gives up on its own well before this
REFRESH = 24 * 3600   # yt-dlp extractors go stale fast; YouTube changes weekly

# options without a value
ALLOWED_FLAGS = {
    "--no-check-certificate", "--no-check-certificates", "--no-cache-dir", "--rm-cache-dir",
    "--get-url", "-g", "--get-title", "-e", "--get-id", "--get-duration", "--get-format",
    "--no-playlist", "--yes-playlist", "--flat-playlist", "-J", "--dump-single-json", "-j", "--dump-json",
    "-i", "--ignore-errors", "-s", "--simulate", "--skip-download", "--no-warnings", "-q", "--quiet",
    "-v", "--verbose", "--no-progress", "-4", "--force-ipv4", "-6", "--force-ipv6", "--geo-bypass",
    "--no-mark-watched", "--live-from-start", "--no-live-from-start", "--no-color", "--ignore-config",
}
# options with one value
ALLOWED_VALUE = {
    "-f", "--format", "-S", "--format-sort", "--extractor-args", "--impersonate", "--user-agent",
    "--referer", "--add-header", "--add-headers", "--socket-timeout", "-R", "--retries", "--extractor-retries",
    "--match-filter", "--match-filters", "--playlist-items", "-I", "--print", "-O", "--proxy",
    "--cookies", "--sub-langs", "--geo-verification-proxy", "--compat-options",
}
# values that are Windows paths
PATH_VALUE = {"--cookies"}
# options that take a value but must not pass: dropped together with their value
DROP_VALUE = {
    "-o", "--output", "-P", "--paths", "--exec", "--exec-before-download", "--config-location",
    "--config-locations", "--plugin-dirs", "-a", "--batch-file", "--netrc-location", "--netrc-cmd",
    "--ffmpeg-location", "--cache-dir", "--cookies-from-browser", "--js-runtimes", "--load-info-json",
    "--download-archive", "--update-to", "--exp-allow", "--postprocessor-args", "--ppa", "--downloader",
    "--external-downloader", "--downloader-args", "--external-downloader-args", "--remote-components",
}
UPDATE = {"-U", "--update", "--no-update"}


def split_cmdline(s):
    """Split a Windows command-line tail the way the MSVC runtime (CommandLineToArgvW) does."""
    args, i, n = [], 0, len(s)
    while True:
        while i < n and s[i] in " \t":
            i += 1
        if i >= n:
            return args
        cur, quoted = [], False
        while i < n and (quoted or s[i] not in " \t"):
            if s[i] == "\\":
                j = i
                while j < n and s[j] == "\\":
                    j += 1
                if j < n and s[j] == '"':
                    cur.append("\\" * ((j - i) // 2))
                    if (j - i) % 2:
                        cur.append('"')
                        i = j + 1
                    else:
                        i = j
                else:
                    cur.append(s[i:j])
                    i = j
            elif s[i] == '"':
                if quoted and i + 1 < n and s[i + 1] == '"':
                    cur.append('"')    # "" inside quotes: a literal quote (MSVC 2008+)
                    i += 2
                else:
                    quoted = not quoted
                    i += 1
            else:
                cur.append(s[i])
                i += 1
        args.append("".join(cur))


def unix_path(p, drive_c):
    """Z:\\home\\x -> /home/x, C:\\users\\... -> <drive_c>/users/...; other strings unchanged."""
    if len(p) >= 2 and p[1] == ":":
        rest = p[2:].replace("\\", "/")
        d = p[0].upper()
        if d == "Z":
            return rest or "/"
        if d == "C" and drive_c:
            return drive_c.rstrip("/") + ("/" + rest.lstrip("/") if rest.strip("/") else "")
    return p


def map_args(args, drive_c=None):
    """VRChat's yt-dlp.exe arguments -> (native yt-dlp arguments, notes, update_only)."""
    out, notes, update = [], [], False
    i = 0
    while i < len(args):
        a = args[i]
        i += 1
        if a.startswith("--") and "=" in a:
            opt, val, inline = a.split("=", 1)[0], a.split("=", 1)[1], True
        else:
            opt, val, inline = a, None, False
        if opt in UPDATE:
            update = True
            continue
        if opt in ALLOWED_FLAGS and not inline:
            out.append(opt)
        elif opt in ALLOWED_VALUE or opt in DROP_VALUE:
            if not inline:
                if i >= len(args):
                    notes.append(f"{opt}: missing value, dropped")
                    continue
                val = args[i]
                i += 1
            if opt in DROP_VALUE:
                notes.append(f"{opt}: not passed to the native yt-dlp")
                continue
            if opt in PATH_VALUE:
                val = unix_path(val, drive_c)
            out += [opt, val]
        elif opt.startswith("-") and opt != "-":
            notes.append(f"{opt}: unknown option, dropped")
        elif a.lower().startswith(("http://", "https://")):
            out.append(a)
        else:
            notes.append(f"{a!r}: not a URL, dropped")
    if out and "--ignore-config" not in out:
        out.insert(0, "--ignore-config")
    return out, notes, update


def ytdlp_path():
    p = os.path.join(DATA, "yt-dlp")
    return p if os.path.isfile(p) else None


def js_runtime_args():
    deno = os.path.join(DATA, "deno")
    return ["--js-runtimes", "deno:" + deno] if os.access(deno, os.X_OK) else []


def refresh(now=None):
    """Fetch the latest yt-dlp zipapp and check it against the release's SHA2-256SUMS."""
    os.makedirs(DATA, exist_ok=True)
    p = os.path.join(DATA, "yt-dlp")
    if os.path.isfile(p) and (now or time.time()) - os.path.getmtime(p) < REFRESH:
        return False
    with urllib.request.urlopen(RELEASE + "SHA2-256SUMS", timeout=30) as r:
        sums = r.read().decode()
    want = next((ln.split()[0] for ln in sums.splitlines() if ln.split()[-1:] == ["yt-dlp"]), None)
    with urllib.request.urlopen(RELEASE + "yt-dlp", timeout=60) as r:
        blob = r.read()
    if not want or hashlib.sha256(blob).hexdigest() != want:
        raise RuntimeError("yt-dlp download does not match SHA2-256SUMS")
    tmp = p + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.chmod(tmp, 0o755)
    os.replace(tmp, p)
    return True


def refresher():
    while True:
        try:
            if refresh():
                print("steam-ytdlp: native yt-dlp updated", flush=True)
        except Exception as e:  # offline, GitHub down: keep the current copy, retry later
            print(f"steam-ytdlp: yt-dlp update failed: {e}", flush=True)
            time.sleep(600)
            continue
        time.sleep(3600)


def frame(kind, data):
    return kind + str(len(data)).encode() + b"\n" + data


class Handler(socketserver.StreamRequestHandler):
    drive_c = None

    def handle(self):
        head = self.rfile.readline(256).split()
        if len(head) != 3 or head[:2] != [b"STEAMAC-YTDLP", b"1"] or not head[2].isdigit() or int(head[2]) > 65536:
            return
        cmd = self.rfile.read(int(head[2])).decode("utf-8", "replace")
        args, notes, update = map_args(split_cmdline(cmd), self.drive_c)
        exe = ytdlp_path()
        if exe is None:
            self.wfile.write(b"F\n")
            return
        err = "".join(f"steamac-ytdlp: {n}\n" for n in notes).encode()
        if update and not args:
            self.wfile.write(frame(b"E", err + b"steamac-ytdlp: native yt-dlp is updated by the guest\n") + b"X0\n")
            return
        t0 = time.monotonic()
        try:
            p = subprocess.run([sys.executable, exe] + js_runtime_args() + args, stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=TIMEOUT)
            out, rc, err = p.stdout, p.returncode, err + p.stderr
        except subprocess.TimeoutExpired:
            out, rc, err = b"", 1, err + b"steamac-ytdlp: native yt-dlp timed out\n"
        print(f"steam-ytdlp: rc {rc} in {time.monotonic() - t0:.1f} s: {' '.join(args)}", flush=True)
        self.wfile.write(frame(b"O", out) + frame(b"E", err) + b"X%d\n" % rc)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def serve(port=PORT, drive_c=None):
    Handler.drive_c = drive_c
    threading.Thread(target=refresher, daemon=True).start()
    with Server(("127.0.0.1", port), Handler) as s:
        print(f"steam-ytdlp: serving native yt-dlp on 127.0.0.1:{port}", flush=True)
        s.serve_forever()


if __name__ == "__main__":
    serve(drive_c=sys.argv[1] if len(sys.argv) > 1 else None)
