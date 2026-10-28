//! Heartbeat: `alive <uptime_ms> <loadavg1>` once a second, so the launcher can
//! tell "the guest is busy" (heartbeats keep coming while the GPU is idle) from
//! "the guest is not responding" (they stop).
//!
//! A 1 s periodic timerfd: the idle loop adds it to its ppoll set, the boot
//! loop drains it on its 200 ms tick. No polling of its own, no CPU between
//! beats. A beat is skipped while the port still holds unsent data (the host
//! is not reading; stale beats must not crowd out real messages).

use std::os::fd::RawFd;

use crate::port::Port;

pub struct Heartbeat {
    fd: RawFd,
}

impl Heartbeat {
    /// None if timerfd is unavailable (then there are no heartbeats).
    pub fn new() -> Option<Heartbeat> {
        let fd = unsafe { libc::timerfd_create(libc::CLOCK_MONOTONIC, libc::TFD_NONBLOCK | libc::TFD_CLOEXEC) };
        if fd < 0 {
            eprintln!("fx-progress: timerfd_create: {}; no heartbeats", std::io::Error::last_os_error());
            return None;
        }
        let second = libc::timespec { tv_sec: 1, tv_nsec: 0 };
        let spec = libc::itimerspec { it_interval: second, it_value: second };
        if unsafe { libc::timerfd_settime(fd, 0, &spec, std::ptr::null_mut()) } < 0 {
            eprintln!("fx-progress: timerfd_settime: {}; no heartbeats", std::io::Error::last_os_error());
            unsafe { libc::close(fd) };
            return None;
        }
        Some(Heartbeat { fd })
    }

    pub fn fd(&self) -> RawFd {
        self.fd
    }

    /// Drain the timer; send one beat if it expired since the last call.
    /// Return whether a tick elapsed, even when backpressure skips the beat.
    pub fn pump(&self, port: &mut Port) -> bool {
        let mut expirations = 0u64;
        let n = unsafe { libc::read(self.fd, &mut expirations as *mut u64 as *mut libc::c_void, 8) };
        let elapsed = n == 8 && expirations > 0;
        if elapsed && !port.has_pending() {
            port.send_quiet(&line());
        }
        elapsed
    }
}

impl Drop for Heartbeat {
    fn drop(&mut self) {
        unsafe { libc::close(self.fd) };
    }
}

/// `alive <ms since boot> <1-minute load average>`.
fn line() -> String {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_BOOTTIME, &mut ts) };
    let uptime_ms = ts.tv_sec as i64 * 1000 + ts.tv_nsec as i64 / 1_000_000;
    let mut info: libc::sysinfo = unsafe { std::mem::zeroed() };
    let load = if unsafe { libc::sysinfo(&mut info) } == 0 {
        info.loads[0] as f64 / 65536.0 // SI_LOAD_SHIFT = 16
    } else {
        0.0
    };
    format!("alive {uptime_ms} {load:.2}")
}
