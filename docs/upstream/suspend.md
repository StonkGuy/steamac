# Suspend

**How:** Settings → General → “When closing the window” → **Suspend** — closing the window then
suspends the VM instead of shutting it down; or use **FX Steam Launcher → Suspend** (Ctrl+Cmd+S,
works even when the guest has the keyboard). `krun_pause` (libkrun patch 0016) stops all vCPUs and
guest audio, the window is hidden, and a ⏸ icon appears in the menu bar: “SteamOS suspended”,
since when and how much memory is in use, **Resume**, **Shut Down SteamOS**. A suspended VM uses
no CPU (~0%); the Mac can sleep.

**Resume:** click the Dock icon, relaunch the app (Finder, `open`, `open -a`), use the menu bar
icon, or choose **Resume** from the menu. The window returns (including full-screen mode, if it was
active), mouse capture is restored; the “Resuming…” progress pill stays until the first new guest
frame (if the guest GPU is idle, as with a static Steam interface, 0.5 s; at most 2.5 s).

**Clocks:** the guest's monotonic clock does not see the pause (libkrun shifts the virtual timer,
as QEMU does) — the sched_ext scheduler and watchdogs do not fire. Immediately after resume,
`fx-clock-sync.service` sets the wall clock (a root service in the layer, using the same
`fx-progress-agent clock-sync` binary, started by udev when the port appears): the launcher writes
its time as `time <unix_ns>` to the virtio port `fx.clock`; the service adjusts only
CLOCK_REALTIME (`clock_adjtime(ADJ_SETOFFSET)`, only forward and only if it lags by more than 1 s).
After that adjustment, timesyncd synchronizes on its own.

**Quit:** Cmd+Q / Dock → Quit while suspended asks “SteamOS is suspended”: **Shut Down SteamOS**
(the guest resumes and shuts down normally) or **Cancel** (it remains suspended). Logging out,
restarting, and shutting down the Mac do not prompt.

**Limitations:** the state lives only in memory while FX Steam Launcher is running — it is not
saved to disk (guest memory and host GPU state — virglrenderer, MoltenVK, Metal — are not
serialized). Quitting the app, an app crash, logging out, or shutting down the Mac is an ordinary
SteamOS shutdown; unsaved game progress is lost. All guest memory remains occupied while the VM
is suspended. Guest network connections (online games, downloads) may drop and reconnect after a
long pause.

