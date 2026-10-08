# Controller

Any controller macOS's GameController framework supports (Xbox, DualSense, DualShock 4, MFi, …)
drives one gamepad in SteamOS; Settings → Controller picks which one. The pad is not a virtio-input
device: the launcher sends it over the virtio-console port `fx.pad` to the guest's root service
`fx-pad.service` (`fx-progress-agent pad`, started by udev when the port appears), which creates it
with uinput. So it follows the Mac while the VM runs: it appears when a controller connects (Steam
shows “Controller Connected”), disappears when the last one goes, and changes kind with it.
Settings → Controller → **Appears in SteamOS as** (`--pad auto|xbox360|dualsense|dualshock4` for
one run):

- **Automatic** (default): the same kind as the controller that drives it — DualSense (or Edge) →
  DualSense, DualShock 4 → DualShock 4, anything else → Xbox 360 controller;
- **Xbox 360 controller**: what the kernel's `xpad` driver exposes (`045e:028e`);
- **DualSense** / **DualShock 4**: what `hid-playstation` / `hid-sony` expose for a USB pad
  (`054c:0ce6` / `054c:09cc`, version `0x8111`, face buttons by position, digital L2/R2 besides the
  analog triggers). Steam's SDL maps it as a PS5 / PS4 controller and shows PlayStation glyphs.

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

`--control-fifo` test commands: `pad on` (a pad without a controller, as `--input-selftest` uses),
`pad off`, `pad test` (A + left stick), `pad state` (the guest's pad, its last rumble level, whether
the guest takes HID devices, connected DualSenses and input reports passed through).

