//! `fx-progress-agent pad`: root system service (fx-pad.service, started by udev when the
//! launcher's virtio-console port `fx.pad` appears) that owns the guest's gamepad. Further
//! players' pads have a port (`fx.pad2`, ...) and an instance of this service each
//! (fx-pad@.service, FX_PAD_PORT). The pad is one of two kinds:
//! - a uinput device the launcher drives from the Mac's controller (GameController); its force
//!   feedback (FF_RUMBLE, what SDL and Steam use for rumble) goes back to the launcher, which
//!   plays it on the controller;
//! - the Mac's HID device itself, passed through as a uhid device (uhid.rs): the guest's own
//!   driver binds to it (hid-playstation for a DualSense: touchpad, motion sensors, lights,
//!   rumble) and Steam reads its /dev/hidraw. Input reports come from the Mac as they are;
//!   output reports and feature/set requests go back to the Mac's device.
//! Unlike a virtio-input device, the pad can come and go and change its identity while the VM
//! runs.
//!
//! Protocol on fx.pad, one message per line:
//!   host -> guest  `create <bus> <vendor> <product> <version> <keys> <axes> <name>`
//!                    ids as 4-digit hex; keys `code,code,...`; axes `code:min:max:fuzz:flat,...`;
//!                    the name is the rest of the line. Replaces the current pad, if any.
//!                  `hid-create <bus> <vendor> <product> <version> <country> <descriptor> <name>`
//!                    a uhid pad: ids hex, the report descriptor in hex, the name the rest of the
//!                    line. Replaces the current pad, if any.
//!                  `remove`                   removes the pad of either kind
//!                  `ev <type>:<code>:<value> ...`   one input frame (EV_KEY / EV_ABS), then SYN_REPORT
//!                  `hid-input <report>`       one input report (hex, report ID first if numbered)
//!                  `hid-get-reply <id> <err> [<report>]`  answers `hid-get` (err: 0 or an errno)
//!                  `hid-set-reply <id> <err>`             answers `hid-set`
//!   guest -> host  `hello`                    at start: no pad exists; the host sends `create` again
//!                  `caps hid`                 right after `hello`: `hid-create` is understood
//!                  `rumble <strong> <weak>`   the pad's combined rumble, 0..65535 each, on every change
//!                  `hid-output <type> <report>`       a driver / hidraw output report for the device
//!                  `hid-get <id> <type> <rnum>`       GET_REPORT, waits for `hid-get-reply <id>`
//!                  `hid-set <id> <type> <report>`     SET_REPORT, waits for `hid-set-reply <id>`
//!                    type: feature | output | input; rnum decimal; reports hex, ID first.
//!
//! uinput leaves force-feedback playback to its user-space driver: uploads and erases arrive as
//! EV_UINPUT requests on the device fd, plays and stops as EV_FF events. `Rumble` implements the
//! kernel's ff-memless semantics for FF_RUMBLE (start after replay.delay, stop after
//! replay.length, 0 = until stopped, `value` repetitions, re-uploading a playing effect restarts
//! it, concurrent effects add up and saturate) on a millisecond clock, testable without a device.
//! A uhid pad has no such thing: its driver sends rumble in its own output reports.
//!
//! Environment overrides: FX_PAD_PORT=<path> (fx-pad@.service: a further player's port; testing),
//! FX_PAD_UINPUT=<path>, FX_PAD_UHID=<path> (testing).

use std::collections::BTreeMap;
use std::ffi::CString;
use std::io;
use std::mem::size_of;
use std::time::Instant;

use crate::codec::{hex, unhex};
use crate::port::Port;
use crate::uhid::{self, HidSpec};

const DEFAULT_PORT: &str = "/dev/virtio-ports/fx.pad";
const DEFAULT_UINPUT: &str = "/dev/uinput";
const DEFAULT_UHID: &str = "/dev/uhid";

const EV_SYN: u16 = 0x00;
const EV_KEY: u16 = 0x01;
const EV_ABS: u16 = 0x03;
const EV_FF: u16 = 0x15;
const EV_UINPUT: u16 = 0x0101;
const SYN_REPORT: u16 = 0;
const UI_FF_UPLOAD: u16 = 1;
const UI_FF_ERASE: u16 = 2;
const FF_RUMBLE: u16 = 0x50;
const KEY_MAX: u16 = 0x2ff;
const ABS_MAX: u16 = 0x3f;
/// Effect slots per pad (ids 0..15); EV_FF codes at or above it are FF_GAIN and the like.
const FF_EFFECTS_MAX: u16 = 16;

// <asm-generic/ioctl.h>: dir << 30 | size << 16 | type << 8 | nr; <linux/uinput.h> numbers.
const fn ioc(dir: u32, nr: u32, size: usize) -> u32 {
    (dir << 30) | ((size as u32) << 16) | ((b'U' as u32) << 8) | nr
}
const W: u32 = 1;
const R: u32 = 2;
const UI_DEV_CREATE: u32 = ioc(0, 1, 0);
const UI_DEV_DESTROY: u32 = ioc(0, 2, 0);
const UI_DEV_SETUP: u32 = ioc(W, 3, size_of::<libc::uinput_setup>());
const UI_ABS_SETUP: u32 = ioc(W, 4, size_of::<libc::uinput_abs_setup>());
const UI_SET_EVBIT: u32 = ioc(W, 100, size_of::<libc::c_int>());
const UI_SET_KEYBIT: u32 = ioc(W, 101, size_of::<libc::c_int>());
const UI_SET_ABSBIT: u32 = ioc(W, 103, size_of::<libc::c_int>());
const UI_SET_FFBIT: u32 = ioc(W, 107, size_of::<libc::c_int>());
const UI_BEGIN_FF_UPLOAD: u32 = ioc(R | W, 200, size_of::<libc::uinput_ff_upload>());
const UI_END_FF_UPLOAD: u32 = ioc(W, 201, size_of::<libc::uinput_ff_upload>());
const UI_BEGIN_FF_ERASE: u32 = ioc(R | W, 202, size_of::<libc::uinput_ff_erase>());
const UI_END_FF_ERASE: u32 = ioc(W, 203, size_of::<libc::uinput_ff_erase>());

// MARK: protocol

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Axis {
    pub code: u16,
    pub min: i32,
    pub max: i32,
    pub fuzz: i32,
    pub flat: i32,
}

/// What `create` asks for: the identity and capabilities of the real controller's kernel driver.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Spec {
    pub bus: u16,
    pub vendor: u16,
    pub product: u16,
    pub version: u16,
    pub keys: Vec<u16>,
    pub axes: Vec<Axis>,
    pub name: String,
}

#[derive(Debug, PartialEq, Eq)]
pub enum Command {
    Create(Spec),
    HidCreate(HidSpec),
    Remove,
    Frame(Vec<(u16, u16, i32)>),
    HidInput(Vec<u8>),
    HidGetReply { id: u32, err: u16, data: Vec<u8> },
    HidSetReply { id: u32, err: u16 },
}

/// One host line; None for anything malformed.
pub fn parse(line: &str) -> Option<Command> {
    let line = line.trim_end_matches(['\r', '\n']);
    let (verb, rest) = line.split_once(' ').unwrap_or((line, ""));
    match verb {
        "remove" if rest.trim().is_empty() => Some(Command::Remove),
        "hid-input" => {
            let report = unhex(rest.trim())?;
            (!report.is_empty() && report.len() <= uhid::DATA_MAX).then_some(Command::HidInput(report))
        }
        "hid-get-reply" => {
            let f: Vec<&str> = rest.split_whitespace().collect();
            if !(2..=3).contains(&f.len()) {
                return None;
            }
            let data = f.get(2).map_or(Some(Vec::new()), |s| unhex(s))?;
            (data.len() <= uhid::DATA_MAX).then_some(())?;
            Some(Command::HidGetReply { id: f[0].parse().ok()?, err: f[1].parse().ok()?, data })
        }
        "hid-set-reply" => {
            let f: Vec<&str> = rest.split_whitespace().collect();
            (f.len() == 2).then_some(())?;
            Some(Command::HidSetReply { id: f[0].parse().ok()?, err: f[1].parse().ok()? })
        }
        "hid-create" => {
            let f: Vec<&str> = rest.splitn(7, ' ').collect();
            if f.len() != 7 {
                return None;
            }
            let hex16 = |s: &str| u16::from_str_radix(s, 16).ok();
            let descriptor = unhex(f[5])?;
            let name = f[6].trim();
            if name.is_empty() || descriptor.is_empty() || descriptor.len() > uhid::DATA_MAX {
                return None;
            }
            Some(Command::HidCreate(HidSpec {
                bus: hex16(f[0])?,
                vendor: hex16(f[1])?,
                product: hex16(f[2])?,
                version: hex16(f[3])?,
                country: u32::from_str_radix(f[4], 16).ok()?,
                descriptor,
                name: name.to_string(),
            }))
        }
        "ev" => {
            let mut events = Vec::new();
            for e in rest.split_whitespace() {
                let mut f = e.split(':');
                let (t, c, v) = (f.next()?.parse().ok()?, f.next()?.parse().ok()?, f.next()?.parse().ok()?);
                if f.next().is_some() || !matches!(t, EV_KEY | EV_ABS) {
                    return None;
                }
                events.push((t, c, v));
            }
            (!events.is_empty()).then_some(Command::Frame(events))
        }
        "create" => {
            let f: Vec<&str> = rest.splitn(7, ' ').collect();
            if f.len() != 7 {
                return None;
            }
            let hex = |s: &str| u16::from_str_radix(s, 16).ok();
            let keys = if f[4] == "-" {
                Vec::new()
            } else {
                f[4].split(',').map(|k| k.parse().ok().filter(|&k: &u16| k <= KEY_MAX)).collect::<Option<Vec<u16>>>()?
            };
            let axes = if f[5] == "-" {
                Vec::new()
            } else {
                f[5].split(',')
                    .map(|a| {
                        let v: Vec<&str> = a.split(':').collect();
                        let n = |i: usize| v.get(i).and_then(|s| s.parse::<i32>().ok());
                        let code = v.first()?.parse::<u16>().ok().filter(|&c| c <= ABS_MAX)?;
                        (v.len() == 5).then_some(())?;
                        Some(Axis { code, min: n(1)?, max: n(2)?, fuzz: n(3)?, flat: n(4)? })
                    })
                    .collect::<Option<Vec<Axis>>>()?
            };
            let name = f[6].trim();
            if name.is_empty() || (keys.is_empty() && axes.is_empty()) {
                return None;
            }
            Some(Command::Create(Spec {
                bus: hex(f[0])?,
                vendor: hex(f[1])?,
                product: hex(f[2])?,
                version: hex(f[3])?,
                keys,
                axes,
                name: name.to_string(),
            }))
        }
        _ => None,
    }
}

// MARK: force feedback

/// An uploaded FF_RUMBLE effect.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Effect {
    pub strong: u16,
    pub weak: u16,
    /// ms; 0 = until stopped.
    pub length: u16,
    /// ms before each play.
    pub delay: u16,
}

#[derive(Clone, Copy, Debug)]
struct Play {
    /// When the current play's rumble starts (ms).
    start: u64,
    /// Plays left, this one included.
    left: i32,
}

struct Slot {
    effect: Effect,
    play: Option<Play>,
}

/// Playback of the pad's FF_RUMBLE effects (ff-memless semantics, see the module doc).
#[derive(Default)]
pub struct Rumble {
    slots: BTreeMap<i16, Slot>,
}

impl Rumble {
    /// New or updated effect `id`; a playing effect restarts with the new parameters.
    pub fn upload(&mut self, id: i16, effect: Effect, now: u64) {
        let play = self.slots.get(&id).and_then(|s| s.play).map(|p| Play { start: now + effect.delay as u64, left: p.left });
        self.slots.insert(id, Slot { effect, play });
    }

    /// EV_FF `id` with `count`: play it that many times (0 = stop).
    pub fn play(&mut self, id: i16, count: i32, now: u64) {
        if let Some(s) = self.slots.get_mut(&id) {
            s.play = (count > 0).then(|| Play { start: now + s.effect.delay as u64, left: count });
        }
    }

    pub fn erase(&mut self, id: i16) {
        self.slots.remove(&id);
    }

    /// Finish plays that ended by `now`, starting the next repetition (after its delay).
    fn advance(&mut self, now: u64) {
        for s in self.slots.values_mut() {
            while let Some(p) = s.play {
                if s.effect.length == 0 {
                    break;
                }
                let end = p.start + s.effect.length as u64;
                if now < end {
                    break;
                }
                s.play = (p.left > 1).then(|| Play { start: end + s.effect.delay as u64, left: p.left - 1 });
            }
        }
    }

    /// Combined (strong, weak) at `now`: the sum of all effects in their rumble phase, saturated.
    pub fn level(&mut self, now: u64) -> (u16, u16) {
        self.advance(now);
        let (mut strong, mut weak) = (0u32, 0u32);
        for s in self.slots.values() {
            if s.play.is_some_and(|p| p.start <= now) {
                strong += s.effect.strong as u32;
                weak += s.effect.weak as u32;
            }
        }
        (strong.min(0xffff) as u16, weak.min(0xffff) as u16)
    }

    /// When `level` can change next without new requests (a play starting or ending).
    pub fn next_change(&mut self, now: u64) -> Option<u64> {
        self.advance(now);
        self.slots
            .values()
            .filter_map(|s| {
                let p = s.play?;
                if now < p.start {
                    Some(p.start)
                } else {
                    (s.effect.length > 0).then(|| p.start + s.effect.length as u64)
                }
            })
            .min()
    }
}

// MARK: uinput

fn ioctl<T>(fd: libc::c_int, request: u32, arg: *mut T) -> io::Result<()> {
    if unsafe { libc::ioctl(fd, request as libc::Ioctl, arg) } < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn ioctl_int(fd: libc::c_int, request: u32, value: u16) -> io::Result<()> {
    if unsafe { libc::ioctl(fd, request as libc::Ioctl, value as libc::c_int) } < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// The uinput device; destroyed on drop (closing the fd would do it too).
struct Device {
    fd: libc::c_int,
}

impl Device {
    fn create(path: &str, spec: &Spec) -> io::Result<Device> {
        let c = CString::new(path).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;
        let fd = unsafe { libc::open(c.as_ptr(), libc::O_RDWR | libc::O_NONBLOCK | libc::O_CLOEXEC) };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let d = Device { fd };
        if !spec.keys.is_empty() {
            ioctl_int(fd, UI_SET_EVBIT, EV_KEY)?;
            for &k in &spec.keys {
                ioctl_int(fd, UI_SET_KEYBIT, k)?;
            }
        }
        if !spec.axes.is_empty() {
            ioctl_int(fd, UI_SET_EVBIT, EV_ABS)?;
            for a in &spec.axes {
                ioctl_int(fd, UI_SET_ABSBIT, a.code)?;
                let mut s: libc::uinput_abs_setup = unsafe { std::mem::zeroed() };
                s.code = a.code;
                s.absinfo.minimum = a.min;
                s.absinfo.maximum = a.max;
                s.absinfo.fuzz = a.fuzz;
                s.absinfo.flat = a.flat;
                ioctl(fd, UI_ABS_SETUP, &mut s)?;
            }
        }
        ioctl_int(fd, UI_SET_EVBIT, EV_FF)?;
        ioctl_int(fd, UI_SET_FFBIT, FF_RUMBLE)?;
        let mut setup: libc::uinput_setup = unsafe { std::mem::zeroed() };
        setup.id.bustype = spec.bus;
        setup.id.vendor = spec.vendor;
        setup.id.product = spec.product;
        setup.id.version = spec.version;
        for (dst, &b) in setup.name.iter_mut().zip(spec.name.as_bytes().iter().take(libc::UINPUT_MAX_NAME_SIZE - 1)) {
            *dst = b as libc::c_char;
        }
        setup.ff_effects_max = FF_EFFECTS_MAX as u32;
        ioctl(fd, UI_DEV_SETUP, &mut setup)?;
        ioctl::<libc::c_void>(fd, UI_DEV_CREATE, std::ptr::null_mut())?;
        Ok(d)
    }

    /// The events and a SYN_REPORT in one write (the kernel takes whole input_event structs).
    fn write_frame(&self, events: &[(u16, u16, i32)]) -> io::Result<()> {
        let mut buf: Vec<libc::input_event> = Vec::with_capacity(events.len() + 1);
        for &(t, c, v) in events.iter().chain(std::iter::once(&(EV_SYN, SYN_REPORT, 0))) {
            let mut e: libc::input_event = unsafe { std::mem::zeroed() };
            e.type_ = t;
            e.code = c;
            e.value = v;
            buf.push(e);
        }
        let bytes = buf.len() * size_of::<libc::input_event>();
        let n = unsafe { libc::write(self.fd, buf.as_ptr() as *const libc::c_void, bytes) };
        if n < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    /// Pending requests and FF events; never blocks.
    fn read_events(&self) -> Vec<libc::input_event> {
        let mut out = Vec::new();
        loop {
            let mut e: libc::input_event = unsafe { std::mem::zeroed() };
            let n = unsafe { libc::read(self.fd, &mut e as *mut _ as *mut libc::c_void, size_of::<libc::input_event>()) };
            if n == size_of::<libc::input_event>() as isize {
                out.push(e);
            } else if n < 0 && io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
                continue;
            } else {
                return out;
            }
        }
    }

    /// Answer an EV_UINPUT request; the kernel's EVIOCSFF / EVIOCRMFF caller waits for it.
    fn serve(&self, code: u16, request_id: u32, rumble: &mut Rumble, now: u64) -> io::Result<()> {
        match code {
            UI_FF_UPLOAD => {
                let mut up: libc::uinput_ff_upload = unsafe { std::mem::zeroed() };
                up.request_id = request_id;
                ioctl(self.fd, UI_BEGIN_FF_UPLOAD, &mut up)?;
                let e = &up.effect;
                up.retval = if e.type_ == FF_RUMBLE {
                    // ff_effect.u is a union; ff_rumble_effect is its first two u16s.
                    let b = e.u[0].to_ne_bytes();
                    let effect = Effect {
                        strong: u16::from_ne_bytes([b[0], b[1]]),
                        weak: u16::from_ne_bytes([b[2], b[3]]),
                        length: e.replay.length,
                        delay: e.replay.delay,
                    };
                    rumble.upload(e.id, effect, now);
                    0
                } else {
                    -libc::EINVAL
                };
                ioctl(self.fd, UI_END_FF_UPLOAD, &mut up)
            }
            UI_FF_ERASE => {
                let mut er: libc::uinput_ff_erase = unsafe { std::mem::zeroed() };
                er.request_id = request_id;
                ioctl(self.fd, UI_BEGIN_FF_ERASE, &mut er)?;
                rumble.erase(er.effect_id as i16);
                er.retval = 0;
                ioctl(self.fd, UI_END_FF_ERASE, &mut er)
            }
            _ => Ok(()),
        }
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        let _ = ioctl::<libc::c_void>(self.fd, UI_DEV_DESTROY, std::ptr::null_mut());
        unsafe { libc::close(self.fd) };
    }
}

// MARK: service

/// The guest's pad: driven by the host (uinput) or the Mac's own HID device (uhid).
enum Pad {
    Uinput(Device),
    Hid(uhid::Device),
}

impl Pad {
    fn fd(&self) -> libc::c_int {
        match self {
            Pad::Uinput(d) => d.fd,
            Pad::Hid(d) => d.fd,
        }
    }
}

/// Serve the port until the host closes it (exit 0); exit 1 if it cannot be opened.
pub fn run() -> i32 {
    let port_path = std::env::var("FX_PAD_PORT").unwrap_or_else(|_| DEFAULT_PORT.into());
    let uinput = std::env::var("FX_PAD_UINPUT").unwrap_or_else(|_| DEFAULT_UINPUT.into());
    let uhid_path = std::env::var("FX_PAD_UHID").unwrap_or_else(|_| DEFAULT_UHID.into());
    let mut port = match Port::open(&port_path) {
        Ok(p) => p,
        Err(e) => {
            eprintln!("fx-pad: {port_path}: {e}");
            return 1;
        }
    };
    let clock = Instant::now();
    let ms = || clock.elapsed().as_millis() as u64;
    let mut pad: Option<Pad> = None;
    let mut rumble = Rumble::default();
    let mut sent = (0u16, 0u16);
    eprintln!("fx-pad: > hello");
    port.send_quiet("hello");
    port.send_quiet("caps hid");
    loop {
        for line in port.read_lines() {
            match parse(&line) {
                Some(Command::Create(spec)) => {
                    pad = None;
                    rumble = Rumble::default();
                    match Device::create(&uinput, &spec) {
                        Ok(d) => {
                            eprintln!("fx-pad: created \"{}\" ({:04x}:{:04x})", spec.name, spec.vendor, spec.product);
                            pad = Some(Pad::Uinput(d));
                        }
                        Err(e) => eprintln!("fx-pad: cannot create \"{}\" on {uinput}: {e}", spec.name),
                    }
                }
                Some(Command::HidCreate(spec)) => {
                    pad = None;
                    rumble = Rumble::default();
                    match uhid::Device::create(&uhid_path, &spec) {
                        Ok(d) => {
                            eprintln!(
                                "fx-pad: created HID \"{}\" ({:04x}:{:04x}:{:04x}, {}-byte descriptor)",
                                spec.name, spec.bus, spec.vendor, spec.product, spec.descriptor.len()
                            );
                            pad = Some(Pad::Hid(d));
                        }
                        Err(e) => eprintln!("fx-pad: cannot create HID \"{}\" on {uhid_path}: {e}", spec.name),
                    }
                }
                Some(Command::Remove) => {
                    if pad.take().is_some() {
                        eprintln!("fx-pad: removed the pad");
                    }
                    rumble = Rumble::default();
                }
                Some(Command::Frame(events)) => {
                    if let Some(Pad::Uinput(d)) = &pad {
                        if let Err(e) = d.write_frame(&events) {
                            eprintln!("fx-pad: write: {e}");
                        }
                    }
                }
                Some(Command::HidInput(report)) => {
                    if let Some(Pad::Hid(d)) = &pad {
                        if let Err(e) = d.write(&uhid::encode_input(&report)) {
                            eprintln!("fx-pad: HID input: {e}");
                        }
                    }
                }
                Some(Command::HidGetReply { id, err, data }) => {
                    if let Some(Pad::Hid(d)) = &pad {
                        if let Err(e) = d.write(&uhid::encode_get_reply(id, err, &data)) {
                            eprintln!("fx-pad: HID get reply {id}: {e}");
                        }
                    }
                }
                Some(Command::HidSetReply { id, err }) => {
                    if let Some(Pad::Hid(d)) = &pad {
                        if let Err(e) = d.write(&uhid::encode_set_reply(id, err)) {
                            eprintln!("fx-pad: HID set reply {id}: {e}");
                        }
                    }
                }
                None => eprintln!("fx-pad: ignoring {:?}", line.trim()),
            }
        }
        match &pad {
            Some(Pad::Uinput(d)) => {
                for e in d.read_events() {
                    let now = ms();
                    match e.type_ {
                        EV_UINPUT => {
                            if let Err(err) = d.serve(e.code, e.value as u32, &mut rumble, now) {
                                eprintln!("fx-pad: force-feedback request {}: {err}", e.code);
                            }
                        }
                        EV_FF if e.code < FF_EFFECTS_MAX => rumble.play(e.code as i16, e.value, now),
                        _ => {}
                    }
                }
            }
            Some(Pad::Hid(d)) => {
                for e in d.read_events() {
                    match e {
                        uhid::Event::Output { rtype, data } => {
                            port.send_quiet(&format!("hid-output {} {}", rtype.name(), hex(&data)))
                        }
                        uhid::Event::GetReport { id, rnum, rtype } => {
                            port.send_quiet(&format!("hid-get {id} {} {rnum}", rtype.name()))
                        }
                        uhid::Event::SetReport { id, rtype, data, .. } => {
                            port.send_quiet(&format!("hid-set {id} {} {}", rtype.name(), hex(&data)))
                        }
                        other => eprintln!("fx-pad: HID {other:?}"),
                    }
                }
            }
            None => {}
        }
        let now = ms();
        let level = if matches!(pad, Some(Pad::Uinput(_))) { rumble.level(now) } else { (0, 0) };
        if level != sent {
            port.send_quiet(&format!("rumble {} {}", level.0, level.1));
            sent = level;
        }
        port.flush();

        // Wait for host lines, uinput requests / FF events / uhid requests, the next rumble
        // change, or (while bytes for the host are waiting) a retry tick.
        let Some(port_fd) = port.read_fd() else { return 0 };
        let mut pfds = [
            libc::pollfd { fd: port_fd, events: libc::POLLIN, revents: 0 },
            libc::pollfd { fd: pad.as_ref().map_or(-1, Pad::fd), events: libc::POLLIN, revents: 0 },
        ];
        let mut wait = rumble.next_change(now).map(|t| t.saturating_sub(now) as i64);
        if port.has_pending() {
            wait = Some(wait.map_or(200, |w| w.min(200)));
        }
        let timeout = wait.map_or(-1, |w| w.clamp(0, i32::MAX as i64) as libc::c_int);
        let n = unsafe { libc::poll(pfds.as_mut_ptr(), pfds.len() as libc::nfds_t, timeout) };
        if n < 0 && io::Error::last_os_error().kind() != io::ErrorKind::Interrupted {
            eprintln!("fx-pad: poll: {}", io::Error::last_os_error());
            return 1;
        }
        if pfds[0].revents & libc::POLLHUP != 0 && pfds[0].revents & libc::POLLIN == 0 {
            eprintln!("fx-pad: host closed the port");
            return 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DS: &str = "create 0003 054c 0ce6 8111 304,305,307 0:-32768:32767:16:128,2:0:255:0:0 \
                      Sony Interactive Entertainment DualSense Wireless Controller";

    #[test]
    fn ioctl_numbers_match_the_kernel_headers() {
        // aarch64 / 64-bit values from <linux/uinput.h>: they encode the struct sizes.
        assert_eq!(UI_DEV_CREATE, 0x5501);
        assert_eq!(UI_DEV_DESTROY, 0x5502);
        assert_eq!(UI_DEV_SETUP, 0x405c5503);
        assert_eq!(UI_ABS_SETUP, 0x401c5504);
        assert_eq!(UI_SET_EVBIT, 0x40045564);
        assert_eq!(UI_SET_FFBIT, 0x4004556b);
        assert_eq!(UI_BEGIN_FF_UPLOAD, 0xc06855c8);
        assert_eq!(UI_END_FF_UPLOAD, 0x406855c9);
        assert_eq!(UI_BEGIN_FF_ERASE, 0xc00c55ca);
        assert_eq!(UI_END_FF_ERASE, 0x400c55cb);
    }

    #[test]
    fn parses_create_with_name_spaces() {
        let Some(Command::Create(s)) = parse(DS) else { panic!("not a create") };
        assert_eq!((s.bus, s.vendor, s.product, s.version), (3, 0x054c, 0x0ce6, 0x8111));
        assert_eq!(s.keys, vec![304, 305, 307]);
        assert_eq!(s.axes[0], Axis { code: 0, min: -32768, max: 32767, fuzz: 16, flat: 128 });
        assert_eq!(s.axes[1].code, 2);
        assert_eq!(s.name, "Sony Interactive Entertainment DualSense Wireless Controller");
    }

    #[test]
    fn rejects_malformed_lines() {
        assert_eq!(parse("create 0003 054c 0ce6 8111 304 0:0:1:0:0"), None, "no name");
        assert_eq!(parse("create 0003 054c 0ce6 8111 304 0:0:1:0 name"), None, "axis with 4 fields");
        assert_eq!(parse("create 0003 054c 0ce6 8111 768 - name"), None, "key code beyond KEY_MAX");
        assert_eq!(parse("create 0003 054c 0ce6 8111 - - name"), None, "no capabilities");
        assert_eq!(parse("create zz03 054c 0ce6 8111 304 - name"), None, "bad hex id");
        assert_eq!(parse("ev 1:304"), None);
        assert_eq!(parse("ev 21:0:1"), None, "only EV_KEY / EV_ABS from the host");
        assert_eq!(parse("ev"), None);
        assert_eq!(parse("remove now"), None);
        assert_eq!(parse("hello"), None);
    }

    #[test]
    fn parses_frames_and_remove() {
        assert_eq!(parse("ev 1:304:1 3:0:-32768\n"), Some(Command::Frame(vec![(1, 304, 1), (3, 0, -32768)])));
        assert_eq!(parse("remove"), Some(Command::Remove));
    }

    #[test]
    fn parses_hid_create_with_descriptor_and_name() {
        let Some(Command::HidCreate(s)) = parse("hid-create 0005 054c 0ce6 0100 21 05010905a101 DualSense Wireless Controller")
        else {
            panic!("not a hid-create")
        };
        assert_eq!((s.bus, s.vendor, s.product, s.version, s.country), (5, 0x054c, 0x0ce6, 0x0100, 0x21));
        assert_eq!(s.descriptor, vec![0x05, 0x01, 0x09, 0x05, 0xa1, 0x01]);
        assert_eq!(s.name, "DualSense Wireless Controller");
    }

    #[test]
    fn parses_hid_reports_and_replies() {
        assert_eq!(parse("hid-input 01807f"), Some(Command::HidInput(vec![0x01, 0x80, 0x7f])));
        assert_eq!(parse("hid-get-reply 7 0 0501"), Some(Command::HidGetReply { id: 7, err: 0, data: vec![0x05, 0x01] }));
        assert_eq!(parse("hid-get-reply 8 5"), Some(Command::HidGetReply { id: 8, err: 5, data: vec![] }), "an error without data");
        assert_eq!(parse("hid-set-reply 9 0"), Some(Command::HidSetReply { id: 9, err: 0 }));
    }

    #[test]
    fn rejects_malformed_hid_lines() {
        assert_eq!(parse("hid-create 0005 054c 0ce6 0100 21 0501"), None, "no name");
        assert_eq!(parse("hid-create 0005 054c 0ce6 0100 21 - name"), None, "no descriptor");
        assert_eq!(parse("hid-create 0005 054c 0ce6 0100 21 050 name"), None, "odd hex");
        let huge = format!("hid-create 0003 054c 0ce6 0100 0 {} name", "00".repeat(uhid::DATA_MAX + 1));
        assert_eq!(parse(&huge), None, "descriptor beyond HID_MAX_DESCRIPTOR_SIZE");
        assert_eq!(parse("hid-input"), None, "empty report");
        assert_eq!(parse(&format!("hid-input {}", "00".repeat(uhid::DATA_MAX + 1))), None, "report beyond UHID_DATA_MAX");
        assert_eq!(parse("hid-get-reply 7"), None);
        assert_eq!(parse("hid-get-reply x 0 01"), None);
        assert_eq!(parse("hid-set-reply 9"), None);
        assert_eq!(parse("hid-set-reply 9 0 1"), None);
    }

    fn fx(strong: u16, weak: u16, length: u16, delay: u16) -> Effect {
        Effect { strong, weak, length, delay }
    }

    #[test]
    fn plays_after_delay_for_length() {
        let mut r = Rumble::default();
        r.upload(0, fx(1000, 2000, 100, 50), 0);
        assert_eq!(r.level(0), (0, 0), "uploaded only");
        r.play(0, 1, 10);
        assert_eq!(r.level(59), (0, 0));
        assert_eq!(r.next_change(59), Some(60));
        assert_eq!(r.level(60), (1000, 2000));
        assert_eq!(r.next_change(60), Some(160));
        assert_eq!(r.level(159), (1000, 2000));
        assert_eq!(r.level(160), (0, 0));
        assert_eq!(r.next_change(160), None);
    }

    #[test]
    fn zero_length_plays_until_stopped() {
        let mut r = Rumble::default();
        r.upload(3, fx(500, 0, 0, 0), 0);
        r.play(3, 1, 0);
        assert_eq!(r.level(1_000_000), (500, 0));
        assert_eq!(r.next_change(1_000_000), None);
        r.play(3, 0, 1_000_001);
        assert_eq!(r.level(1_000_001), (0, 0));
    }

    #[test]
    fn repeats_with_delay_before_each_play() {
        let mut r = Rumble::default();
        r.upload(1, fx(100, 100, 10, 5), 0);
        r.play(1, 2, 0);
        assert_eq!(r.level(5), (100, 100));
        assert_eq!(r.level(15), (0, 0), "gap: the second play waits its delay");
        assert_eq!(r.next_change(15), Some(20));
        assert_eq!(r.level(20), (100, 100));
        assert_eq!(r.level(30), (0, 0));
        assert_eq!(r.next_change(30), None);
    }

    #[test]
    fn concurrent_effects_add_up_and_saturate() {
        let mut r = Rumble::default();
        r.upload(0, fx(40000, 1000, 0, 0), 0);
        r.upload(1, fx(40000, 2000, 50, 0), 0);
        r.play(0, 1, 0);
        r.play(1, 1, 0);
        assert_eq!(r.level(10), (0xffff, 3000));
        assert_eq!(r.level(50), (40000, 1000), "effect 1 ended");
    }

    #[test]
    fn reupload_restarts_a_playing_effect() {
        // SDL updates its rumble by uploading the same id again (and playing it).
        let mut r = Rumble::default();
        r.upload(0, fx(1000, 0, 100, 0), 0);
        r.play(0, 1, 0);
        r.upload(0, fx(3000, 0, 100, 0), 80);
        assert_eq!(r.level(80), (3000, 0));
        assert_eq!(r.level(150), (3000, 0), "the new parameters run their full length from the update");
        assert_eq!(r.level(180), (0, 0));
        r.upload(0, fx(5000, 0, 100, 0), 200);
        assert_eq!(r.level(200), (0, 0), "an idle effect stays idle when updated");
    }

    #[test]
    fn stop_and_erase_silence_an_effect() {
        let mut r = Rumble::default();
        r.upload(0, fx(1000, 0, 0, 0), 0);
        r.upload(1, fx(2000, 0, 0, 0), 0);
        r.play(0, 1, 0);
        r.play(1, 1, 0);
        r.play(0, 0, 10);
        assert_eq!(r.level(10), (2000, 0));
        r.erase(1);
        assert_eq!(r.level(10), (0, 0));
        r.play(1, 1, 20);
        assert_eq!(r.level(20), (0, 0), "an erased effect cannot be played");
    }
}
