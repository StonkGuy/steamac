//! The virtio-console port `fx.progress` (one message per line, both ways).
//!
//! The port is opened `O_NONBLOCK`, so a host that is not reading (or an older
//! launcher whose port has no reader) can never stall the session: unwritten
//! bytes stay in a small bounded buffer and are retried on the next tick.
//! Host -> guest lines (including `cursor-track 1|0`) are read without blocking too.

use std::ffi::CString;
use std::io;
use std::time::{Duration, Instant};

/// Upper bound for bytes waiting for the host. The X cursor bitmap is the
/// largest message (a base64 line); if the host stops reading this long, older
/// messages are dropped (only the newest state matters to the overlay).
const MAX_PENDING: usize = 512 * 1024;

/// Optional overlay handshake. Unknown/malformed commands do not change tracking.
pub fn cursor_tracking(line: &str) -> Option<bool> {
    let (command, arg) = line.trim().split_once(' ')?;
    if command != "cursor-track" {
        return None;
    }
    match arg.trim() {
        "1" => Some(true),
        "0" => Some(false),
        _ => None,
    }
}

pub struct Port {
    fd: libc::c_int,
    pending: Vec<u8>,
    /// Host -> guest bytes of an incomplete line.
    rx: Vec<u8>,
    /// The port can be read (a character device opened read-write).
    readable: bool,
}

impl Port {
    pub fn open(path: &str) -> io::Result<Port> {
        let c = CString::new(path).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?;
        // O_APPEND only matters for the regular-file test override.
        let flags = libc::O_NONBLOCK | libc::O_CLOEXEC | libc::O_NOCTTY | libc::O_APPEND;
        let mut fd = unsafe { libc::open(c.as_ptr(), libc::O_RDWR | flags) };
        let mut readable = fd >= 0;
        if fd < 0 {
            fd = unsafe { libc::open(c.as_ptr(), libc::O_WRONLY | flags) };
        }
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        // A regular file (FX_PROGRESS_PORT test override) would read back our own lines.
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        if unsafe { libc::fstat(fd, &mut st) } == 0 && (st.st_mode & libc::S_IFMT) == libc::S_IFREG {
            readable = false;
        }
        Ok(Port { fd, pending: Vec::new(), rx: Vec::new(), readable })
    }

    /// For ppoll(POLLIN) while the port is readable.
    pub fn read_fd(&self) -> Option<libc::c_int> {
        self.readable.then_some(self.fd)
    }

    /// The host side hung up (POLLHUP): stop polling for input.
    pub fn stop_reading(&mut self) {
        self.readable = false;
    }

    /// Complete host -> guest lines received so far; never blocks.
    pub fn read_lines(&mut self) -> Vec<String> {
        if !self.readable {
            return Vec::new();
        }
        let mut buf = [0u8; 4096];
        loop {
            let n = unsafe { libc::read(self.fd, buf.as_mut_ptr() as *mut libc::c_void, buf.len()) };
            if n > 0 {
                self.rx.extend_from_slice(&buf[..n as usize]);
                continue;
            }
            if n < 0 && io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
                continue;
            }
            break; // EAGAIN, EOF (host not connected) or an error: try again later
        }
        let mut lines = Vec::new();
        while let Some(nl) = self.rx.iter().position(|&b| b == b'\n') {
            lines.push(String::from_utf8_lossy(&self.rx[..nl]).to_string());
            self.rx.drain(..=nl);
        }
        // Profile sets contain all games, not just one title. Bound incomplete input
        // without cutting off the normal multi-game snapshot at the old HID limit.
        if self.rx.len() > 1024 * 1024 {
            self.rx.clear();
        }
        lines
    }

    /// Queue one protocol line (without the trailing newline) and try to send it.
    pub fn send(&mut self, line: &str) {
        eprintln!("fx-progress: > {line}");
        self.send_quiet(line);
    }

    /// `send` without the journal line (heartbeats: one per second).
    pub fn send_quiet(&mut self, line: &str) {
        if self.pending.len() + line.len() + 1 > MAX_PENDING {
            self.pending.clear();
        }
        self.pending.extend_from_slice(line.as_bytes());
        self.pending.push(b'\n');
        self.flush();
    }

    /// Write as much of the queue as the port accepts right now; never blocks.
    pub fn flush(&mut self) {
        while !self.pending.is_empty() {
            let n = unsafe {
                libc::write(self.fd, self.pending.as_ptr() as *const libc::c_void, self.pending.len())
            };
            if n > 0 {
                self.pending.drain(..n as usize);
                continue;
            }
            let err = io::Error::last_os_error();
            if n < 0 && err.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            // EAGAIN (host not reading yet) or any other error: keep the data,
            // retry on the next tick.
            break;
        }
    }

    /// Bulk transfer (log bundle): queued messages, then every line in full,
    /// waiting for the host to read for at most `timeout` overall. Returns
    /// false if it did not all go out (the rest is dropped; a newline ends a
    /// partly written line so the next message starts clean).
    pub fn send_bulk(&mut self, lines: &[String], timeout: Duration) -> bool {
        let deadline = Instant::now() + timeout;
        let mut buf = std::mem::take(&mut self.pending);
        for l in lines {
            buf.extend_from_slice(l.as_bytes());
            buf.push(b'\n');
        }
        let mut off = 0;
        while off < buf.len() {
            let n = unsafe { libc::write(self.fd, buf[off..].as_ptr() as *const libc::c_void, buf.len() - off) };
            if n > 0 {
                off += n as usize;
                continue;
            }
            let err = io::Error::last_os_error();
            if n < 0 && err.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            if n < 0 && err.kind() != io::ErrorKind::WouldBlock {
                break;
            }
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                break;
            }
            let mut p = libc::pollfd { fd: self.fd, events: libc::POLLOUT, revents: 0 };
            unsafe { libc::poll(&mut p, 1, left.as_millis().min(1000) as libc::c_int) };
        }
        let ok = off == buf.len();
        if !ok && off > 0 && buf[off - 1] != b'\n' {
            self.pending.push(b'\n');
        }
        ok
    }

    pub fn has_pending(&self) -> bool {
        !self.pending.is_empty()
    }
}

impl Drop for Port {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cursor_tracking_requires_an_explicit_boolean_command() {
        assert_eq!(cursor_tracking("cursor-track 1"), Some(true));
        assert_eq!(cursor_tracking("cursor-track 0"), Some(false));
        assert_eq!(cursor_tracking("  cursor-track   1 \r"), Some(true));
        for line in ["", "cursor-track", "cursor-track 2", "cursor-track true",
            "cursor-track 1 extra", "cursor-tracking 1", "cursor 1", "future-command 1"] {
            assert_eq!(cursor_tracking(line), None, "{line:?}");
        }
    }
}
