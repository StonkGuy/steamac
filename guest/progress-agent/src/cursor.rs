//! The focused game's cursor, for the DXMT path's cursor overlay.
//!
//! The host opts in with `cursor-track 1` only while a wmtr game layer is visible.
//! Until then (including with an older host), this module does no X work. Position
//! follows the game window at ~60 Hz; XFixes CursorNotify drives bitmap/visibility
//! updates without image polling. The game's blank cursor, not gamescope's auto-hide
//! timer, controls visibility.
//!
//! Messages: `cursor 0|1`, `cursor-image <w> <h> <xhot> <yhot> <base64 BGRA>`,
//! `cursor-pos <x> <y> <win_w> <win_h>`. Image and position precede `cursor 1`.

use std::collections::HashMap;
use std::os::fd::AsRawFd;
use std::os::unix::fs::MetadataExt;
use std::time::{Duration, Instant};
use x11rb::connection::Connection;
use x11rb::errors::ConnectError;
use x11rb::protocol::xfixes::{ConnectionExt as _, CursorNotifyMask};
use x11rb::protocol::xproto::{AtomEnum, ChangeWindowAttributesAux, ConnectionExt, EventMask, Window};
use x11rb::protocol::Event;
use x11rb::rust_connection::RustConnection;

use crate::codec;
use crate::port::Port;

struct Conn {
    display: String,
    conn: RustConnection,
    root: Window,
    active_atom: u32,
    image_dirty: bool,
}

enum Fail {
    // Match clipx11: authentication rejection / foreign servers are not retried.
    NotOurs,
    Retry,
}

const MAX_CURSOR_DIMENSION: u16 = 256;
const POINTER_INTERVAL: Duration = Duration::from_micros(16_667);
const SELECT_INTERVAL: Duration = Duration::from_millis(250);
const SCAN_INTERVAL: Duration = Duration::from_secs(5);

#[derive(PartialEq)]
struct Image {
    width: u16,
    height: u16,
    xhot: u16,
    yhot: u16,
    pixels: Vec<u32>,
}

impl Image {
    fn visible(&self) -> bool {
        self.width > 1 && self.height > 1
            && self.width <= MAX_CURSOR_DIMENSION && self.height <= MAX_CURSOR_DIMENSION
            && self.pixels.len() == usize::from(self.width) * usize::from(self.height)
            && self.pixels.iter().any(|p| p >> 24 != 0)
    }
}

#[derive(Debug, PartialEq, Clone, Copy)]
struct Pos {
    x: i16,
    y: i16,
    width: u16,
    height: u16,
}

#[derive(PartialEq, Clone, Copy)]
struct Target {
    conn: usize,
    window: Window,
    width: u16,
    height: u16,
}

#[derive(Default)]
struct Sent {
    visible: Option<bool>,
    image: Option<Image>,
    pos: Option<Pos>,
}

impl Sent {
    fn hide(&mut self, mut send: impl FnMut(&str)) {
        if self.visible != Some(false) {
            send("cursor 0");
        }
        self.visible = Some(false);
        self.image = None;
        self.pos = None;
    }

    fn publish(&mut self, image: Option<Image>, pos: Pos, mut send: impl FnMut(&str)) {
        if let Some(image) = image {
            if !image.visible() {
                self.hide(send);
                return;
            }
            if self.image.as_ref() != Some(&image) {
                // XFixes ARGB pixels are premultiplied BGRA bytes on the little-endian guest.
                let mut bytes = Vec::with_capacity(image.pixels.len() * 4);
                for pixel in &image.pixels {
                    bytes.extend_from_slice(&pixel.to_le_bytes());
                }
                send(&format!("cursor-image {} {} {} {} {}",
                    image.width, image.height, image.xhot, image.yhot, codec::base64(&bytes)));
                self.image = Some(image);
            }
        }
        if self.image.is_none() {
            return;
        }
        if self.pos != Some(pos) {
            send(&format!("cursor-pos {} {} {} {}", pos.x, pos.y, pos.width, pos.height));
            self.pos = Some(pos);
        }
        if self.visible != Some(true) {
            send("cursor 1");
            self.visible = Some(true);
        }
    }
}

pub struct Cursor {
    tracking: bool,
    conns: Vec<Conn>,
    /// Socket identity permits a replacement server at the same display to be reconsidered.
    rejected: HashMap<String, (u64, u64)>,
    next_scan: Instant,
    next_poll: Instant,
    next_select: Instant,
    target: Option<Target>,
    sent: Sent,
}

impl Cursor {
    pub fn new() -> Cursor {
        Cursor {
            tracking: false,
            conns: Vec::new(),
            rejected: HashMap::new(),
            next_scan: Instant::now(),
            next_poll: Instant::now(),
            next_select: Instant::now(),
            target: None,
            sent: Sent::default(),
        }
    }

    /// A repeated enable is the reconnect handshake: the new host needs a full snapshot.
    pub fn set_tracking(&mut self, enabled: bool) {
        self.tracking = enabled;
        self.target = None;
        self.sent = Sent::default();
        if !enabled {
            self.conns.clear();
        } else {
            for c in &mut self.conns {
                c.image_dirty = true;
            }
        }
        let now = Instant::now();
        self.next_scan = now;
        self.next_poll = now;
        self.next_select = now;
    }

    pub fn poll_fds(&self, pfds: &mut Vec<libc::pollfd>) {
        for c in &self.conns {
            pfds.push(libc::pollfd { fd: c.conn.stream().as_raw_fd(), events: libc::POLLIN, revents: 0 });
        }
    }

    /// No timer or cursor X connections while the host has not requested tracking.
    pub fn wait_ms(&self) -> Option<i64> {
        if !self.tracking {
            return None;
        }
        let mut deadline = self.next_scan.min(self.next_select);
        if self.target.is_some() {
            deadline = deadline.min(self.next_poll);
        }
        let left = deadline.saturating_duration_since(Instant::now());
        // Round up: a sub-millisecond remainder must not busy-loop.
        Some(left.as_nanos().div_ceil(1_000_000) as i64)
    }

    fn drop_conn(&mut self, i: usize, port: &mut Port) {
        self.conns.remove(i);
        self.target = None;
        self.sent.hide(|line| port.send_quiet(line));
        self.next_select = Instant::now();
    }

    fn connect(&mut self) {
        let Ok(dir) = std::fs::read_dir("/tmp/.X11-unix") else { return };
        let mut present = HashMap::new();
        for e in dir.flatten() {
            let name = e.file_name();
            let Some(n) = name.to_str().and_then(|s| s.strip_prefix('X')) else { continue };
            if n.is_empty() || !n.bytes().all(|b| b.is_ascii_digit()) {
                continue;
            }
            let Ok(meta) = e.metadata() else { continue };
            let identity = (meta.dev(), meta.ino());
            let display = format!(":{n}");
            present.insert(display.clone(), identity);
            if self.conns.iter().any(|c| c.display == display) || self.rejected.get(&display) == Some(&identity) {
                continue;
            }
            match connect(&display) {
                Ok(c) => {
                    self.conns.push(c);
                    self.next_select = Instant::now();
                }
                Err(Fail::NotOurs) => { self.rejected.insert(display, identity); }
                Err(Fail::Retry) => {}
            }
        }
        self.rejected.retain(|display, identity| present.get(display) == Some(identity));
    }

    /// Only an active game's client pixels are a safe mapping target, never a random X root.
    fn select_target(&self) -> Option<Target> {
        self.conns.iter().enumerate().find_map(|(i, c)| {
            let active = c.conn.get_property(false, c.root, c.active_atom, AtomEnum::WINDOW, 0, 1)
                .ok()?.reply().ok()?;
            let window = active.value32()?.next().filter(|&w| w != 0)?;
            let class = c.conn.get_property(false, window, AtomEnum::WM_CLASS, AtomEnum::STRING, 0, 1024)
                .ok()?.reply().ok()?;
            if !game_class(&class.value) {
                return None;
            }
            let geometry = c.conn.get_geometry(window).ok()?.reply().ok()?;
            if geometry.width == 0 || geometry.height == 0 {
                return None;
            }
            Some(Target { conn: i, window, width: geometry.width, height: geometry.height })
        })
    }

    pub fn pump(&mut self, port: &mut Port) {
        if !self.tracking {
            return;
        }
        let now = Instant::now();
        if now >= self.next_scan {
            self.next_scan = now + SCAN_INTERVAL;
            self.connect();
        }
        // Drain before considering the pointer timer: cursor hiding must not wait for a poll.
        let mut i = 0;
        while i < self.conns.len() {
            let mut failed = false;
            loop {
                match self.conns[i].conn.poll_for_event() {
                    Ok(Some(Event::XfixesCursorNotify(_))) => self.conns[i].image_dirty = true,
                    Ok(Some(Event::PropertyNotify(e))) if e.atom == self.conns[i].active_atom => {
                        self.next_select = now;
                    }
                    Ok(Some(Event::ConfigureNotify(_))) | Ok(Some(Event::DestroyNotify(_))) => {
                        self.next_select = now;
                    }
                    Ok(Some(_)) => {}
                    Ok(None) => break,
                    Err(_) => { failed = true; break; }
                }
            }
            if failed {
                self.drop_conn(i, port);
            } else {
                i += 1;
            }
        }
        if now >= self.next_select {
            self.next_select = now + SELECT_INTERVAL;
            let target = self.select_target();
            if target != self.target {
                self.sent.hide(|line| port.send_quiet(line));
                if let Some(t) = target {
                    self.conns[t.conn].image_dirty = true;
                }
                self.next_poll = now;
                self.target = target;
            }
        }
        let Some(target) = self.target else {
            self.sent.hide(|line| port.send_quiet(line));
            return;
        };
        let dirty = self.conns[target.conn].image_dirty;
        if now < self.next_poll && !dirty {
            return;
        }
        self.next_poll = now + POINTER_INTERVAL;
        let c = &self.conns[target.conn];
        let reply = c.conn.query_pointer(target.window).ok().and_then(|cookie| cookie.reply().ok());
        let Some(reply) = reply else {
            self.drop_conn(target.conn, port);
            return;
        };
        if !reply.same_screen {
            self.sent.hide(|line| port.send_quiet(line));
            self.conns[target.conn].image_dirty = true;
            return;
        }
        let pos = Pos { x: reply.win_x, y: reply.win_y, width: target.width, height: target.height };
        let image = if dirty {
            let img = c.conn.xfixes_get_cursor_image().ok().and_then(|cookie| cookie.reply().ok());
            let Some(img) = img else {
                self.drop_conn(target.conn, port);
                return;
            };
            self.conns[target.conn].image_dirty = false;
            Some(Image { width: img.width, height: img.height, xhot: img.xhot, yhot: img.yhot, pixels: img.cursor_image })
        } else {
            None
        };
        self.sent.publish(image, pos, |line| port.send_quiet(line));
    }
}

fn game_class(class: &[u8]) -> bool {
    class.split(|&b| b == 0).any(|name| name.starts_with(b"steam_app_"))
}

fn connect(display: &str) -> Result<Conn, Fail> {
    let (conn, screen) = RustConnection::connect(Some(display)).map_err(|e| match e {
        ConnectError::SetupAuthenticate(_) | ConnectError::SetupFailed(_) => Fail::NotOurs,
        _ => Fail::Retry,
    })?;
    let root = conn.setup().roots.get(screen).ok_or(Fail::NotOurs)?.root;
    let server_id = conn.intern_atom(false, b"GAMESCOPE_XWAYLAND_SERVER_ID")
        .map_err(|_| Fail::Retry)?.reply().map_err(|_| Fail::Retry)?.atom;
    let prop = conn.get_property(false, root, server_id, AtomEnum::CARDINAL, 0, 1)
        .map_err(|_| Fail::Retry)?.reply().map_err(|_| Fail::Retry)?;
    if prop.value.is_empty() {
        return Err(Fail::NotOurs);
    }
    let active_atom = conn.intern_atom(false, b"_NET_ACTIVE_WINDOW")
        .map_err(|_| Fail::Retry)?.reply().map_err(|_| Fail::Retry)?.atom;
    conn.xfixes_query_version(5, 0).map_err(|_| Fail::NotOurs)?.reply().map_err(|_| Fail::NotOurs)?;
    conn.xfixes_select_cursor_input(root, CursorNotifyMask::DISPLAY_CURSOR)
        .map_err(|_| Fail::Retry)?.check().map_err(|_| Fail::Retry)?;
    conn.change_window_attributes(root, &ChangeWindowAttributesAux::new()
        .event_mask(EventMask::PROPERTY_CHANGE | EventMask::SUBSTRUCTURE_NOTIFY))
        .map_err(|_| Fail::Retry)?.check().map_err(|_| Fail::Retry)?;
    conn.flush().map_err(|_| Fail::Retry)?;
    Ok(Conn { display: display.into(), conn, root, active_atom, image_dirty: true })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn image(pixel: u32) -> Image {
        Image { width: 2, height: 2, xhot: 1, yhot: 0, pixels: vec![pixel; 4] }
    }

    fn pos() -> Pos { Pos { x: -3, y: 12, width: 1280, height: 720 } }

    #[test]
    fn tracking_defaults_off_and_toggles_clear_cached_state() {
        let mut cursor = Cursor::new();
        assert!(!cursor.tracking);
        assert_eq!(cursor.wait_ms(), None);
        let mut pfds = Vec::new();
        cursor.poll_fds(&mut pfds);
        assert!(pfds.is_empty());
        cursor.set_tracking(true);
        assert!(cursor.wait_ms().is_some());
        cursor.sent.image = Some(image(0xff112233));
        cursor.sent.pos = Some(pos());
        cursor.sent.visible = Some(true);
        // A repeated enable (host reconnect) must resend image, position and visibility.
        cursor.set_tracking(true);
        assert!(cursor.sent.image.is_none());
        assert!(cursor.sent.pos.is_none());
        assert_eq!(cursor.sent.visible, None);
        cursor.set_tracking(false);
        assert!(cursor.conns.is_empty());
        assert!(cursor.target.is_none());
        assert_eq!(cursor.wait_ms(), None);
    }

    #[test]
    fn snapshot_precedes_visibility_and_unchanged_state_is_silent() {
        let mut sent = Sent::default();
        let mut lines = Vec::new();
        sent.publish(Some(image(0xff112233)), pos(), |l| lines.push(l.to_owned()));
        assert_eq!(lines[0], "cursor-image 2 2 1 0 MyIR/zMiEf8zIhH/MyIR/w==");
        assert_eq!(lines[1], "cursor-pos -3 12 1280 720");
        assert_eq!(lines[2], "cursor 1");
        lines.clear();
        sent.publish(Some(image(0xff112233)), pos(), |l| lines.push(l.to_owned()));
        sent.publish(None, pos(), |l| lines.push(l.to_owned()));
        assert!(lines.is_empty());
        sent.publish(None, Pos { x: 8, ..pos() }, |l| lines.push(l.to_owned()));
        assert_eq!(lines, ["cursor-pos 8 12 1280 720"]);
    }

    #[test]
    fn blank_cursor_hides_immediately_and_next_show_resynchronizes() {
        let mut sent = Sent::default();
        let mut lines = Vec::new();
        sent.publish(Some(image(0xff112233)), pos(), |_| {});
        sent.publish(Some(image(0)), pos(), |l| lines.push(l.to_owned()));
        sent.publish(None, pos(), |l| lines.push(l.to_owned()));
        assert_eq!(lines, ["cursor 0"]);
        lines.clear();
        sent.publish(Some(image(0xff112233)), pos(), |l| lines.push(l.to_owned()));
        assert!(lines[0].starts_with("cursor-image "));
        assert!(lines[1].starts_with("cursor-pos "));
        assert_eq!(lines[2], "cursor 1");
    }

    #[test]
    fn losing_the_game_target_hides_once_and_forgets_the_old_snapshot() {
        let mut sent = Sent::default();
        sent.publish(Some(image(0xff112233)), pos(), |_| {});
        let mut lines = Vec::new();
        sent.hide(|l| lines.push(l.to_owned()));
        sent.hide(|l| lines.push(l.to_owned()));
        assert_eq!(lines, ["cursor 0"]);
        assert!(sent.image.is_none());
        assert!(sent.pos.is_none());
        lines.clear();
        sent.publish(None, pos(), |l| lines.push(l.to_owned()));
        assert!(lines.is_empty());
    }

    #[test]
    fn image_limits_and_game_class_boundaries() {
        assert!(image(0xff000000).visible());
        assert!(!image(0).visible());
        let mut img = Image { width: 1, height: 1, xhot: 0, yhot: 0, pixels: vec![0xffffffff] };
        assert!(!img.visible());
        img.width = 256;
        img.height = 256;
        img.pixels = vec![0xffffffff; 256 * 256];
        assert!(img.visible());
        img.width = 257;
        img.pixels = vec![0xffffffff; 257 * 256];
        assert!(!img.visible());
        img = image(0xffffffff);
        img.pixels.pop();
        assert!(!img.visible());
        assert!(game_class(b"wine\0steam_app_221100\0"));
        assert!(game_class(b"steam_app_570\0steam_app_570\0"));
        assert!(!game_class(b"Steam\0steam\0"));
        assert!(!game_class(b"not_steam_app_570\0"));
        assert!(!game_class(b""));
    }
}
