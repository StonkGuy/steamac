# Controller

Any controller macOS's GameController framework supports (Xbox, DualSense, DualShock 4, MFi, …)
drives a gamepad in SteamOS: the first connected one, or the one picked in Settings → Controller,
is player 1, and further ones are players 2–4 (“Several controllers” below). The pad is not a virtio-input
device: the launcher sends it over the virtio-console port `fx.pad` to the guest's root service
`fx-pad.service` (`fx-progress-agent pad`, started by udev when the port appears), which creates it
with uinput. So it follows the Mac while the VM runs: it appears when a controller connects (Steam
shows “Controller Connected”), disappears when its controller goes, and changes kind with it.
Settings → Controller → **Appears in SteamOS as** (`--pad auto|xbox360|dualsense|dualshock4` for
one run):

- **Automatic** (default): the same kind as the controller that drives it — DualSense (or Edge) →
  DualSense, DualShock 4 → DualShock 4, anything else → Xbox 360 controller;
- **Xbox 360 controller**: what the kernel's `xpad` driver exposes (`045e:028e`);
- **DualSense** / **DualShock 4**: what `hid-playstation` / `hid-sony` expose for a USB pad
  (`054c:0ce6` / `054c:09cc`, version `0x8111`, face buttons by position, digital L2/R2 besides the
  analog triggers). Steam's SDL maps it as a PS5 / PS4 controller and shows PlayStation glyphs.

**Home / PS button diagnostics.** The launcher requests that macOS leave this button to SteamOS,
but macOS can override that preference. If it works over USB but not Bluetooth, compare
Settings → Controller's live **Home** indicator and send a report after pressing and releasing
the button on each connection. The launcher log records whether GameController exposes Home,
its system-gesture binding, button and profile callbacks, and whether the Home event was queued
for the guest pad port; `pad state` includes the current Home state. This distinguishes host input
handling from guest delivery without assuming that a Bluetooth failure is a DualSense passthrough
problem (DualShock 4 uses the virtual pad).

**Rumble.** The pad has `FF_RUMBLE`, like the real drivers, so SDL and Steam rumble it — games
through Steam Input reach it via Steam's virtual Xbox pad. uinput leaves playback to its user-space
driver: `fx-pad` implements the kernel's ff-memless rules (delay, length, repetitions, re-upload,
effects adding up) and sends the combined level, `rumble <strong> <weak>`, to the launcher, which
plays it with GameController haptics: the strong motor on the left handle, the weak one on the
right (one level everywhere on controllers without separate handles); nothing while the VM is
paused. Port protocol: `guest/progress-agent/src/pad.rs`. Touchpad, gyro, lightbar and adaptive
triggers are HID features of the real controller that this evdev device does not carry.

**DualSense passthrough.** When a DualSense (or Edge) drives the pad and it appears as a
DualSense, Settings → Controller → **Pass a DualSense through** (on by default, applies now) gives
SteamOS the controller itself instead of the uinput pad. The launcher opens it as a raw HID device
(IOHIDManager, without seizing it: GameController still selects it and wakes a sleeping guest) and
`fx-pad` recreates it with `/dev/uhid`: same report descriptor, vendor/product, and USB or Bluetooth
bus. The guest's `hid-playstation` driver binds to it as to a plugged-in controller (gamepad,
touchpad, motion sensors, lightbar and player LEDs, mute LED) and Steam uses its own HIDAPI
DualSense driver on `/dev/hidraw*`, so Steam Input gets touchpad, gyro and the mute button, and
drives rumble, lightbar and adaptive triggers itself. Input reports go to the guest as they are
(`hid-input`); output reports, GET_REPORT and SET_REPORT go back to the controller (`hid-output`,
`hid-get` / `hid-get-reply`, `hid-set` / `hid-set-reply`, hex with the report ID first). While the
guest falls behind, older input reports are dropped instead of queued: each carries the whole
state. Swap A/B and the stick dead zone do not apply to a passed-through controller. GameController
does not say which HID device a controller is: with several DualSenses connected, the first one
found is passed through. Older guest layers without `caps hid` keep getting the uinput pad.

**Several controllers.** With Settings → Controller → **Additional controllers** (on by default,
applies now) further connected controllers are players 2–4, each with a gamepad of its own in
SteamOS, for local co-op. Every player has a virtio-console port: `fx.pad` for player 1, as above,
and `fx.pad2` … `fx.pad4`, each served by an instance of `fx-pad@.service` named after its port
(the same `fx-progress-agent pad` with `FX_PAD_PORT`, started by udev), so the protocol on a port
is the same for every player, and a pad's rumble plays on the controller that drives it. A player
gets a controller once the guest's service of that port said `hello`: a guest layer without
`fx-pad@.service` keeps the one pad. The controller chosen in Settings is player 1; a player keeps
their controller until it disconnects (the others do not move up), and a reconnected controller
takes the first free player. **Appears in SteamOS as**, swap A/B and dead zone apply to every pad (with
Automatic each pad is the kind of its own controller). Only player 1's
DualSense is passed through, and not while another player's controller is a DualSense too
(GameController does not say which HID device is whose): both get uinput pads then. While the VM
runs, the live input test in Settings → Controller badges each controller with its player
(**Player 2**). Off: one controller drives the one pad, as before.

`--control-fifo` test commands: `pad on` (a pad without a controller, as `--input-selftest` uses),
`pad off`, `pad test` (A + left stick) — each takes a player number (`pad on 2`; 1 if not given) —
and `pad state` (the guest's pads, their last rumble level, whether the guest takes HID devices,
connected DualSenses and input reports passed through).
