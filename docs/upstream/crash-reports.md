# Crash reports (Sentry)

The launcher sends crash reports and a few errors to the developers' own Sentry server
(`sentry.fxgam.es`, sentry-cocoa 9.30.0 SDK via SwiftPM). Enabled by default; disable it with the
**Send crash reports and diagnostics** checkbox in Settings → General, in the first-launch window,
or in the Create SteamOS Disk window (the “What is sent” link displays the list below). When disabled,
the SDK does not start at all and makes no network connections (reports already saved on disk remain
there and are not sent). For one launch: `--no-crash-reports` or `STEAMAC_SENTRY=0`.

What is sent:

- Crashes of the supervisor and VM processes (signal/abort, unhandled exceptions): cause, thread
  stacks, list of loaded libraries. This includes Metal/MoltenVK asserts, libkrun/virglrenderer
  aborts, and Rust panics propagated through the libkrun C API. A VM process crash report is sent on
  the next VM launch;
- A few errors (no more than once per fingerprint per process, subject to a global limit; the same
  fingerprint is not sent again for at least a day, or 30 days for shader compilation errors): the
  guest GPU context becomes fatal/device lost (vkr “fatal decoder state”, “device lost”), MoltenVK
  pipeline compilation errors (`[mvk-error] … compile failed`; if MoltenVK printed MSL source
  (`[mvk-msl] …`: the first 40 lines and lines around the error), it is sent in extra `msl_source`,
  not in breadcrumbs; the vkr line “pipeline … creation failed on host” is a breadcrumb only), a
  Rust panic in libkrun (`thread … panicked at`), failure of initial disk provisioning (`provision
  failed`), failure to create a disk, unexpected VM exit (nonzero exit code or signal when shutdown
  was not requested by the user), and the idle indicator's “SteamOS is not responding”;
- VM exit due to SIGTERM/SIGINT/SIGHUP (logging out, `kill`, ^C before handlers are installed) is not
  a crash: only a log line, with no event and no Report a Problem window. Neither is SIGPIPE (both
  processes ignore it: a closed output pipe — the terminal, or the supervisor's stderr channel after
  the supervisor was killed — only fails the write; the VM process then logs to
  `~/Library/Logs/es.fxgam.steamac/steamac-vm.log` or drops the lines, and lets the guest finish
  shutting down; STEAMAC-10). SIGKILL generates a
  warning-level `vm-killed` event, “VM process killed (SIGKILL — memory pressure or force quit)”: 
  whether the kernel killed it for memory (jetsam, `NOTE_EXIT_DETAIL` from the supervisor's kqueue),
  VM memory and the VM process footprint at exit/peak, host `vm_stat` numbers (free/compressed/wired,
  swap), `kern.memorystatus_level`, and the history of memory pressure levels since launch
  (transitions are also written to the log: `memory pressure: …`). Force Quit from the app menu is a
  requested exit, not a report;
- Every event includes the last ~200 lines of launcher stderr as breadcrumbs (`[steamac-vm]`,
  `[mvk-*]`, libkrun/virglrenderer warnings, startup stages) and tags: version
  (`es.fxgam.steamac@<CFBundleShortVersionString>+<git sha>`), environment, and `build_kind` tag:
  `release` — only a `dist.sh` build with notarization (Info.plist `SteamacDistTeamID`, and the
  running code's signature is Developer ID for that team, checked with `SecCodeCheckValidity`),
  `source-build` — any other `.app` (`bundle.sh`, ad-hoc/re-signed copies, `dist.sh --no-notarize`),
  `development` — `work/out/steamac-vm` outside the bundle; macOS, Mac model, GPU, vCPU/RAM, display
  mode, build UUIDs of libkrun / virglrenderer / MoltenVK, `MVK_PATCH_REVISION`, kernel version,
  SteamOS BUILD_ID and layer release (from initramfs lines), the `appid` of the game in guest focus
  (while it is in focus), and a random installation ID.

Not sent: guest console (hvc0), user and computer names (`/Users/<name>` → `~`, name and hostname are
redacted), IP address (`sendDefaultPii=false`, the server does not expose IP addresses), locale/time
zone, Steam account, game titles (App ID only), or files. The supervisor routes its stderr through a
channel (everything still appears in the terminal/log), so it also sees the last lines from a crashed
VM process.

Testing: `--sentry-test-event` (a test event from the supervisor and VM process; the VM process also
sends a pending crash report and exits), `--sentry-test-crash abort|segv|metal|panic|kill|term|shader`
(the VM process crashes: `abort()` inside a C call, `EXC_BAD_ACCESS` in `memset`, Metal assert, Rust
panic in `krun_start_enter` due to an overly long kernel command line — `panic` requires `--kernel`
and, if necessary, `--initrd`; `kill`/`term` — the VM process kills itself with SIGKILL (`vm-killed`
report) or SIGTERM (no report or window); `shader` — prints an example MoltenVK compilation error
with `[mvk-msl]` and a vkr line, then exits). These events have `environment=development` and the
`test=true` tag. `STEAMAC_SENTRY_DEBUG=1` prints the SDK debug log (server responses).

Symbols: `build.sh` puts dSYMs for `steamac-vm` and bundled libraries in `work/out/dSYMs` (libkrun,
virglrenderer, and MoltenVK are built without DWARF — they contain only symbol tables). `dist.sh`
uploads them and the app binaries with `sentry-cli --url https://sentry.fxgam.es debug-files upload`
if `SENTRY_AUTH_TOKEN`, `SENTRY_ORG`, and `SENTRY_PROJECT` are set; otherwise it logs that the upload
was skipped.

