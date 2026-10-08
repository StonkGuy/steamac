# Update check

When the app starts (once per launch, not on every VM reboot, and no more than once every 6 hours —
even across launches), the launcher requests the latest release in the background without delaying
startup from `https://api.github.com/repos/fxgl/steamac/releases/latest` (unauthenticated, 10 s
timeout; drafts and prereleases are excluded) and compares its `vX.Y[.Z]` tag with its own version
(`CFBundleShortVersionString`, numerically: 1.3.10 > 1.3.9). If a new version is available, a window
appears next to the VM window (not over it if there is room on screen; without taking focus — the
keyboard and captured mouse remain with the VM): “FX Steam Launcher X.Y is available — you have …”,
with the release description and buttons **Download** (opens the release's `.dmg` in a browser, or
the release page otherwise), **Skip This Version** (this version is no longer offered at startup),
and **Remind Me Later** (offers it again at the next check). The app menu's **Check for Updates…**
item (while the available version has not been skipped, “Update Available: X.Y…” with a New badge,
opens this window) checks immediately — without the 6-hour limit and including skipped versions —
and reports “You're up to date” or an error; network and HTTP errors at startup are only logged
(`update: …`). Disable it with the **Check for updates at startup** checkbox in Settings → General
(effective immediately). The dev launcher `work/out/steamac-vm` does not check at startup (the menu
works); source builds (`bundle.sh`) and releases do.

Privacy: the request goes only to `api.github.com` (GitHub) and includes only `User-Agent:
FXSteamLauncher/<version>` (plus standard HTTP headers and the previous response's ETag, so GitHub can
reply “not modified”); no Mac or user identifiers, cookies, or Sentry. The check time, ETag, response,
and skipped version are stored in settings (`updateLastCheck`, `updateETag`, `updateCachedBody`,
`updateCachedURL`, `updateSkippedVersion`).

Tests: `STEAMAC_UPDATE_URL` overrides the URL (release JSON or a `/releases` array, `http(s)://` or
`file://`; it also enables the startup check for the dev launcher), `STEAMAC_FAKE_VERSION` overrides
the launcher version; FIFO `--control-fifo`: `update check|startup|state`, `update press
download|skip|later|ok|releases`, `update dump PNG` (the window and `-with-vm.png` — together with the
VM window, as shown on screen).

