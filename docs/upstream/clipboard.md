# Clipboard

Settings → General → **Share clipboard with SteamOS** (on by default, applies now): text (UTF-8)
and PNG images copied on the Mac can be pasted in SteamOS (Ctrl+V — in Steam's text fields, games
and Desktop Mode apps) and the other way round; limits 1 MiB of text and 16 MiB per image (larger
items are skipped with a log line). Items that password managers mark as concealed or transient
(`org.nspasteboard.ConcealedType` / `TransientType`) stay on the Mac unless **Include concealed
(password manager) items** is on. The launcher checks the pasteboard's `changeCount` on a 0.5 s
timer only while the app is active and the VM runs, and once on every activation — never in the
background, while suspended or asleep; images from SteamOS land on the Mac as PNG + TIFF.

Transport: the virtio-console port `fx.clipboard`, framed binary messages (`HELLO` / `STATE` /
`SET` with sequence numbers / `ACK`; `host/launcher/Sources/steamac-vm/Clipboard.swift`,
`guest/progress-agent/src/clipboard.rs`). In the guest the user service
`fx-clipboard-agent.service` (`fx-progress-agent clipboard`, wanted by the gaming and the Desktop
Mode session) owns and watches `CLIPBOARD` (XFixes; TARGETS, UTF8_STRING, text/plain;charset=utf-8,
TEXT, STRING, image/png, INCR above 256 KiB) on **both** gamescope Xwayland servers (`:0` Steam,
`:1` games): gamescope syncs plain text between them itself by taking the selection over, but not
images or INCR-sized text. In Desktop Mode it also uses the Plasma session's Wayland clipboard
through `zwlr_data_control_manager_v1` (`ext_data_control_manager_v1` if present); KWin bridges it
to X11 apps there while an X11 window is active. Echo suppression is by content: each side
remembers the content it last sent or took over, so one copy is one transfer however often
gamescope, KWin or Klipper re-announce it. The selection present when a display appears (e.g.
Klipper's restored history) is not sent to the Mac; the shared content is offered there instead.
`--control-fifo` has `chord KEYCODE ctrl` (e.g. `chord 9 ctrl` = Ctrl+V in the guest) and
`set shareClipboard on|off`.

