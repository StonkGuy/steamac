# SteamOS sleep

Steam → Power → **Sleep**, Steam's idle auto-sleep (Settings → Power → “Sleep after”, 1 hour by
default), and `systemctl suspend` in the guest do not put the guest kernel to sleep (there is
nothing to wake s2idle in a VM — SteamOS previously hung this way until the app exited). The layer
replaces `ExecStart` in `systemd-suspend.service` (and `systemd-suspend-then-hibernate` /
`systemd-hybrid-sleep` likewise; hibernation is disabled in `sleep.conf.d`) with
`fx-progress-agent sleep`: it runs the `system-sleep` hooks (`pre`), writes
`sleep <action> <token>` to the virtio port `fx.sleep`, and waits for a response. The launcher
suspends the VM (`krun_pause`, as with Suspend — CPU ~0%, the Mac can sleep), but the window stays
open: a “SteamOS is sleeping” card overlays the frame. A click, keypress, gamepad button, or Dock
icon wakes it: `krun_resume`, `wake <token> <unix_ns>` is sent to the guest, the command adjusts
the wall clock (as with clock-sync), runs the `post` hooks, and exits — logind sends
PrepareForSleep(false), Steam wakes; the “Waking up…” progress pill stays until the first frame.
The click or keypress that woke the guest is not passed to the guest.

Closing the window during sleep follows “When closing the window”: Suspend hides the window
(Resume subsequently wakes it too); Shut Down / Cmd+Q wakes the guest and presses the power
button once the guest's sleep task has finished (`awake <token>`; logind ignores the button while
it is running). Without the port (`--headless`, old launcher), guest sleep fails instead of
sleeping.

For tests with `--control-fifo`: `close`, `suspend`, `resume` (also wakes a sleeping guest),
`reopen`, `quit`, `wake` (like waking the Mac), `quit-prompt shutdown|cancel|dump PNG`,
`status open|close|dump PNG|item TITLE`.

