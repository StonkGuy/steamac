//! Shared clipboard on a Wayland display through the data-control protocol
//! (ext_data_control_manager_v1, or zwlr_data_control_manager_v1 which KWin
//! 6.2 has): Desktop Mode's nested Plasma session (see clipboard.rs). KWin
//! bridges its Wayland selection to its own Xwayland, so X11 apps there
//! (Steam) are covered too.
//!
//! A minimal hand-written client of the Wayland wire protocol (Unix socket,
//! 32-bit words, file descriptors as SCM_RIGHTS): wl_display, wl_registry,
//! wl_seat and the four data-control interfaces, which have the same requests
//! and events in both protocol variants. Our data source offers an extra mime
//! type (MARKER) so its own selection is recognised and never read back.

use std::collections::{HashMap, VecDeque};
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::net::UnixStream;
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::clipboard::{capped, mailbox, poll_in, Cmd, Content, Ev, Inbox, IMAGE_MAX, MIME_PNG, MIME_TEXT, TAG, TEXT_MAX};

const MARKER: &str = "application/x-fx-steamac-clipboard";
const TEXT_MIMES: [&str; 5] = [MIME_TEXT, "UTF8_STRING", "text/plain", "STRING", "TEXT"];
const STALL: Duration = Duration::from_secs(4);
const CONNECT_TRIES: u32 = 10;

const DISPLAY: u32 = 1;
const REGISTRY: u32 = 2;
const SYNC: u32 = 3;

#[derive(Clone, Copy, PartialEq, Eq)]
enum Obj {
    Device,
    Source,
    Offer,
    Other,
}

enum Arg<'a> {
    U(u32),
    S(&'a str),
    Fd(RawFd),
}

struct Wl {
    sock: UnixStream,
    rx: Vec<u8>,
    fds: VecDeque<OwnedFd>,
    next: u32,
}

impl Wl {
    fn new_id(&mut self) -> u32 {
        let id = self.next;
        self.next += 1;
        id
    }

    fn request(&mut self, obj: u32, op: u16, args: &[Arg]) -> io::Result<()> {
        let mut body = Vec::new();
        let mut fds = Vec::new();
        for a in args {
            match a {
                Arg::U(v) => body.extend_from_slice(&v.to_ne_bytes()),
                Arg::S(s) => {
                    body.extend_from_slice(&(s.len() as u32 + 1).to_ne_bytes());
                    body.extend_from_slice(s.as_bytes());
                    body.push(0);
                    while body.len() % 4 != 0 {
                        body.push(0);
                    }
                }
                Arg::Fd(fd) => fds.push(*fd),
            }
        }
        let mut msg = Vec::with_capacity(8 + body.len());
        msg.extend_from_slice(&obj.to_ne_bytes());
        msg.extend_from_slice(&((((8 + body.len()) as u32) << 16) | op as u32).to_ne_bytes());
        msg.extend_from_slice(&body);
        send_with_fds(self.sock.as_raw_fd(), &msg, &fds)
    }

    /// Read what is available (never blocks); false = the compositor hung up.
    fn fill(&mut self) -> io::Result<bool> {
        let mut buf = [0u8; 4096];
        let space = unsafe { libc::CMSG_SPACE((28 * std::mem::size_of::<RawFd>()) as u32) } as usize;
        let mut cbuf = vec![0u8; space];
        loop {
            let mut iov = libc::iovec { iov_base: buf.as_mut_ptr() as *mut libc::c_void, iov_len: buf.len() };
            let mut mh: libc::msghdr = unsafe { std::mem::zeroed() };
            mh.msg_iov = &mut iov;
            mh.msg_iovlen = 1;
            mh.msg_control = cbuf.as_mut_ptr() as *mut libc::c_void;
            mh.msg_controllen = space as _;
            let n = unsafe { libc::recvmsg(self.sock.as_raw_fd(), &mut mh, libc::MSG_DONTWAIT | libc::MSG_CMSG_CLOEXEC) };
            if n < 0 {
                let e = io::Error::last_os_error();
                return match e.kind() {
                    io::ErrorKind::WouldBlock => Ok(true),
                    io::ErrorKind::Interrupted => continue,
                    _ => Err(e),
                };
            }
            if n == 0 {
                return Ok(false);
            }
            unsafe {
                let mut c = libc::CMSG_FIRSTHDR(&mh);
                while !c.is_null() {
                    if (*c).cmsg_level == libc::SOL_SOCKET && (*c).cmsg_type == libc::SCM_RIGHTS {
                        let data = libc::CMSG_DATA(c) as *const RawFd;
                        let count = ((*c).cmsg_len as usize - libc::CMSG_LEN(0) as usize) / std::mem::size_of::<RawFd>();
                        for i in 0..count {
                            self.fds.push_back(OwnedFd::from_raw_fd(std::ptr::read_unaligned(data.add(i))));
                        }
                    }
                    c = libc::CMSG_NXTHDR(&mh, c);
                }
            }
            self.rx.extend_from_slice(&buf[..n as usize]);
        }
    }

    /// Next complete event: (object, opcode, body).
    fn event(&mut self) -> Option<(u32, u16, Vec<u8>)> {
        if self.rx.len() < 8 {
            return None;
        }
        let obj = u32::from_ne_bytes(self.rx[0..4].try_into().unwrap());
        let w = u32::from_ne_bytes(self.rx[4..8].try_into().unwrap());
        let size = (w >> 16) as usize;
        if size < 8 || self.rx.len() < size {
            return None;
        }
        let body = self.rx[8..size].to_vec();
        self.rx.drain(..size);
        Some((obj, (w & 0xffff) as u16, body))
    }
}

fn send_with_fds(sock: RawFd, msg: &[u8], fds: &[RawFd]) -> io::Result<()> {
    let mut iov = libc::iovec { iov_base: msg.as_ptr() as *mut libc::c_void, iov_len: msg.len() };
    let mut mh: libc::msghdr = unsafe { std::mem::zeroed() };
    mh.msg_iov = &mut iov;
    mh.msg_iovlen = 1;
    let space = unsafe { libc::CMSG_SPACE(std::mem::size_of_val(fds) as u32) } as usize;
    let mut cbuf = vec![0u8; space];
    if !fds.is_empty() {
        mh.msg_control = cbuf.as_mut_ptr() as *mut libc::c_void;
        mh.msg_controllen = space as _;
        unsafe {
            let c = libc::CMSG_FIRSTHDR(&mh);
            (*c).cmsg_level = libc::SOL_SOCKET;
            (*c).cmsg_type = libc::SCM_RIGHTS;
            (*c).cmsg_len = libc::CMSG_LEN(std::mem::size_of_val(fds) as u32) as _;
            std::ptr::copy_nonoverlapping(fds.as_ptr(), libc::CMSG_DATA(c) as *mut RawFd, fds.len());
        }
    }
    loop {
        let n = unsafe { libc::sendmsg(sock, &mh, libc::MSG_NOSIGNAL) };
        if n >= 0 {
            return if n as usize == msg.len() { Ok(()) } else { Err(io::Error::from(io::ErrorKind::WriteZero)) };
        }
        let e = io::Error::last_os_error();
        if e.kind() != io::ErrorKind::Interrupted {
            return Err(e);
        }
    }
}

struct Body<'a> {
    b: &'a [u8],
    at: usize,
}

impl Body<'_> {
    fn u32(&mut self) -> Option<u32> {
        let v = u32::from_ne_bytes(self.b.get(self.at..self.at + 4)?.try_into().ok()?);
        self.at += 4;
        Some(v)
    }
    fn string(&mut self) -> Option<String> {
        let len = self.u32()? as usize;
        let s = self.b.get(self.at..self.at + len)?;
        self.at += len.div_ceil(4) * 4;
        Some(String::from_utf8_lossy(s.strip_suffix(&[0]).unwrap_or(s)).into_owned())
    }
}

struct Session {
    wl: Wl,
    name: String,
    manager: u32,
    device: u32,
    objs: HashMap<u32, Obj>,
    /// Mime types of the offers the compositor announced.
    offers: HashMap<u32, Vec<String>>,
    selection: Option<u32>,
    /// The device's first selection event is the state before we came: not shared.
    initial: bool,
    sources: HashMap<u32, Arc<Content>>,
    tx: Sender<Ev>,
}

pub fn run(path: String, tx: Sender<Ev>) {
    let mut tries = 0;
    let mut s = loop {
        match connect(&path, tx.clone()) {
            Ok(s) => break s,
            Err((retry, why)) => {
                tries += 1;
                if !retry || tries >= CONNECT_TRIES {
                    eprintln!("{TAG}: {path}: {why}; not shared");
                    let _ = tx.send(Ev::Down(path));
                    return;
                }
                std::thread::sleep(Duration::from_secs(1));
            }
        }
    };
    let (mb, inbox) = match mailbox() {
        Ok(m) => m,
        Err(e) => {
            eprintln!("{TAG}: {path}: eventfd: {e}");
            let _ = tx.send(Ev::Down(path));
            return;
        }
    };
    if tx.send(Ev::Up(path.clone(), mb)).is_ok() {
        if let Err(e) = s.serve(&inbox) {
            eprintln!("{TAG}: {path}: connection closed ({e})");
        }
    }
    let _ = tx.send(Ev::Down(path));
}

/// Err((retry, reason)).
fn connect(path: &str, tx: Sender<Ev>) -> Result<Session, (bool, String)> {
    let sock = UnixStream::connect(path).map_err(|e| (true, format!("cannot connect: {e}")))?;
    let mut wl = Wl { sock, rx: Vec::new(), fds: VecDeque::new(), next: 4 };
    let io = |e: io::Error| (true, e.to_string());
    wl.request(DISPLAY, 1, &[Arg::U(REGISTRY)]).map_err(io)?; // get_registry
    wl.request(DISPLAY, 0, &[Arg::U(SYNC)]).map_err(io)?; // sync
    let mut globals: Vec<(u32, String, u32)> = Vec::new();
    let deadline = Instant::now() + STALL;
    'roundtrip: loop {
        if Instant::now() > deadline {
            return Err((true, "no answer from the compositor".into()));
        }
        poll_in(&[wl.sock.as_raw_fd()], Some(Duration::from_millis(200)));
        if !wl.fill().map_err(io)? {
            return Err((true, "compositor hung up".into()));
        }
        while let Some((obj, op, body)) = wl.event() {
            let mut b = Body { b: &body, at: 0 };
            match (obj, op) {
                (REGISTRY, 0) => {
                    if let (Some(name), Some(iface), Some(ver)) = (b.u32(), b.string(), b.u32()) {
                        globals.push((name, iface, ver));
                    }
                }
                (SYNC, 0) => break 'roundtrip,
                (DISPLAY, 0) => return Err((false, "protocol error".into())),
                _ => {}
            }
        }
    }
    let find = |iface: &str| globals.iter().find(|g| g.1 == iface).map(|g| g.0);
    let manager_global = ["ext_data_control_manager_v1", "zwlr_data_control_manager_v1"]
        .into_iter()
        .find_map(|i| find(i).map(|n| (n, i)));
    let Some((mname, miface)) = manager_global else {
        return Err((false, "the compositor has no data-control protocol".into()));
    };
    let Some(sname) = find("wl_seat") else { return Err((false, "no seat".into())) };
    let seat = wl.new_id();
    let manager = wl.new_id();
    let device = wl.new_id();
    wl.request(REGISTRY, 0, &[Arg::U(sname), Arg::S("wl_seat"), Arg::U(1), Arg::U(seat)]).map_err(io)?;
    wl.request(REGISTRY, 0, &[Arg::U(mname), Arg::S(miface), Arg::U(1), Arg::U(manager)]).map_err(io)?;
    wl.request(manager, 1, &[Arg::U(device), Arg::U(seat)]).map_err(io)?; // get_data_device
    eprintln!("{TAG}: {path}: Wayland clipboard through {miface}");
    let mut objs = HashMap::new();
    objs.insert(seat, Obj::Other);
    objs.insert(manager, Obj::Other);
    objs.insert(device, Obj::Device);
    Ok(Session {
        wl,
        name: path.to_string(),
        manager,
        device,
        objs,
        offers: HashMap::new(),
        selection: None,
        initial: true,
        sources: HashMap::new(),
        tx,
    })
}

impl Session {
    fn serve(&mut self, inbox: &Inbox) -> io::Result<()> {
        loop {
            let rev = poll_in(&[self.wl.sock.as_raw_fd(), inbox.wake.fd()], None);
            inbox.wake.drain();
            while let Ok(cmd) = inbox.rx.try_recv() {
                match cmd {
                    Cmd::Own(c) => self.own(c)?,
                }
            }
            if rev[0] != 0 && !self.wl.fill()? {
                return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "compositor hung up"));
            }
            while let Some((obj, op, body)) = self.wl.event() {
                self.dispatch(obj, op, &body)?;
            }
        }
    }

    fn dispatch(&mut self, obj: u32, op: u16, body: &[u8]) -> io::Result<()> {
        let mut b = Body { b: body, at: 0 };
        if obj == DISPLAY {
            match op {
                0 => {
                    let (o, code, msg) = (b.u32(), b.u32(), b.string());
                    return Err(io::Error::other(format!("protocol error on {o:?}: {code:?} {msg:?}")));
                }
                1 => {
                    if let Some(id) = b.u32() {
                        self.objs.remove(&id);
                    }
                }
                _ => {}
            }
            return Ok(());
        }
        match (self.objs.get(&obj).copied().unwrap_or(Obj::Other), op) {
            (Obj::Device, 0) => {
                if let Some(id) = b.u32() {
                    self.objs.insert(id, Obj::Offer);
                    self.offers.insert(id, Vec::new());
                }
            }
            (Obj::Offer, 0) => {
                if let (Some(m), Some(list)) = (b.string(), self.offers.get_mut(&obj)) {
                    list.push(m);
                }
            }
            (Obj::Device, 1) => {
                let id = b.u32().unwrap_or(0);
                self.selection_changed(id)?;
            }
            (Obj::Device, 2) => return Err(io::Error::other("data device finished")),
            (Obj::Device, 3) => {
                // primary_selection (version 2 only): not shared.
                if let Some(id) = b.u32().filter(|&id| id != 0) {
                    self.destroy_offer(id)?;
                }
            }
            (Obj::Source, 0) => {
                let mime = b.string().unwrap_or_default();
                let fd = self.wl.fds.pop_front();
                if let (Some(fd), Some(c)) = (fd, self.sources.get(&obj)) {
                    write_out(fd, &mime, c.clone());
                }
            }
            (Obj::Source, 1) => {
                // cancelled: another selection replaced ours.
                // KWin can queue more than one cancellation for the same source.
                // Destroy it once: a second destructor is a fatal protocol error.
                // Keep Obj::Source until delete_id so queued send events still
                // consume and close their file descriptors.
                if self.sources.remove(&obj).is_some() {
                    self.wl.request(obj, 1, &[])?; // destroy
                }
            }
            _ => {}
        }
        Ok(())
    }

    fn destroy_offer(&mut self, id: u32) -> io::Result<()> {
        if self.offers.remove(&id).is_some() {
            self.wl.request(id, 1, &[])?; // destroy
        }
        Ok(())
    }

    fn selection_changed(&mut self, id: u32) -> io::Result<()> {
        // Offers other than the new selection are of no use any more.
        let stale: Vec<u32> = self.offers.keys().copied().filter(|&o| o != id).collect();
        for o in stale {
            self.destroy_offer(o)?;
        }
        self.selection = (id != 0).then_some(id);
        let initial = std::mem::replace(&mut self.initial, false);
        let Some(mimes) = self.offers.get(&id).cloned() else { return Ok(()) };
        if initial || mimes.iter().any(|m| m == MARKER) {
            return Ok(()); // the state before we came, or our own source
        }
        let text = TEXT_MIMES.iter().find(|m| mimes.iter().any(|x| x == *m)).copied();
        let png = mimes.iter().any(|m| m == MIME_PNG);
        let text = match text {
            Some(m) => self.receive(id, m, TEXT_MAX)?.map(|t| {
                if m == "STRING" {
                    t.iter().map(|&b| b as char).collect::<String>().into_bytes()
                } else {
                    String::from_utf8_lossy(&t).into_owned().into_bytes()
                }
            }),
            None => None,
        };
        let png = if png { self.receive(id, MIME_PNG, IMAGE_MAX)? } else { None };
        if let Some(c) = capped(text, png, &self.name) {
            let _ = self.tx.send(Ev::Guest(self.name.clone(), c));
        }
        Ok(())
    }

    /// Read one mime type of an offer (blocking, bounded by STALL per read).
    /// Data over `cap` is logged and dropped.
    fn receive(&mut self, offer: u32, mime: &str, cap: usize) -> io::Result<Option<Vec<u8>>> {
        let mut p = [0 as RawFd; 2];
        if unsafe { libc::pipe2(p.as_mut_ptr(), libc::O_CLOEXEC) } != 0 {
            return Err(io::Error::last_os_error());
        }
        let (r, w) = unsafe { (OwnedFd::from_raw_fd(p[0]), OwnedFd::from_raw_fd(p[1])) };
        self.wl.request(offer, 0, &[Arg::S(mime), Arg::Fd(w.as_raw_fd())])?;
        drop(w);
        let mut file = std::fs::File::from(r);
        let mut data = Vec::new();
        let mut buf = vec![0u8; 64 * 1024];
        loop {
            if poll_in(&[file.as_raw_fd()], Some(STALL))[0] == 0 {
                eprintln!("{TAG}: {}: the clipboard owner did not send {mime}; change not shared", self.name);
                return Ok(None);
            }
            match file.read(&mut buf) {
                Ok(0) => return Ok(Some(data)),
                Ok(n) => {
                    data.extend_from_slice(&buf[..n]);
                    if data.len() > cap {
                        eprintln!(
                            "{TAG}: {}: copied {mime} is larger than the {} limit: not shared",
                            self.name,
                            crate::clipboard::size(cap)
                        );
                        return Ok(None);
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::Interrupted => {}
                Err(_) => return Ok(None),
            }
        }
    }

    fn own(&mut self, c: Arc<Content>) -> io::Result<()> {
        let src = self.wl.new_id();
        self.wl.request(self.manager, 0, &[Arg::U(src)])?; // create_data_source
        self.objs.insert(src, Obj::Source);
        let mut mimes: Vec<&str> = Vec::new();
        if c.text.is_some() {
            mimes.extend(TEXT_MIMES);
        }
        if c.png.is_some() {
            mimes.push(MIME_PNG);
        }
        mimes.push(MARKER);
        for m in mimes {
            self.wl.request(src, 0, &[Arg::S(m)])?; // offer
        }
        self.wl.request(self.device, 0, &[Arg::U(src)])?; // set_selection
        self.sources.insert(src, c);
        Ok(())
    }
}

/// Answer a paste: write the data for `mime` to `fd` on a short-lived thread
/// (the reader may be slow; the event loop must not wait for it).
fn write_out(fd: OwnedFd, mime: &str, c: Arc<Content>) {
    let data: Option<Arc<Vec<u8>>> = if mime == MIME_PNG {
        c.png.clone()
    } else if mime == "STRING" {
        c.text.as_ref().map(|t| {
            Arc::new(String::from_utf8_lossy(t).chars().map(|ch| if (ch as u32) < 256 { ch as u8 } else { b'?' }).collect())
        })
    } else if TEXT_MIMES.contains(&mime) {
        c.text.clone()
    } else {
        None
    };
    let _ = std::thread::Builder::new().name("clip send".into()).spawn(move || {
        let raw = fd.as_raw_fd();
        unsafe {
            let fl = libc::fcntl(raw, libc::F_GETFL);
            libc::fcntl(raw, libc::F_SETFL, fl & !libc::O_NONBLOCK);
        }
        let mut f = std::fs::File::from(fd);
        if let Some(d) = data {
            let _ = f.write_all(&d);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn source_session() -> (Session, UnixStream) {
        let (sock, peer) = UnixStream::pair().unwrap();
        let (tx, _) = std::sync::mpsc::channel();
        let mut objs = HashMap::new();
        objs.insert(7, Obj::Source);
        let mut sources = HashMap::new();
        sources.insert(7, Arc::new(Content::new(Some(b"old clipboard".to_vec()), None).unwrap()));
        (Session {
            wl: Wl { sock, rx: Vec::new(), fds: VecDeque::new(), next: 8 },
            name: "test".into(), manager: 5, device: 6, objs,
            offers: HashMap::new(), selection: None, initial: false, sources, tx,
        }, peer)
    }

    #[test]
    fn duplicate_source_cancellation_sends_one_destroy() {
        let (mut s, mut peer) = source_session();
        s.dispatch(7, 1, &[]).unwrap();
        s.dispatch(7, 1, &[]).unwrap();
        assert!(s.sources.is_empty());
        // Keep the interface until delete_id so a queued send still consumes its fd.
        assert!(s.objs.get(&7) == Some(&Obj::Source));
        let mut destroy = [0u8; 8];
        peer.read_exact(&mut destroy).unwrap();
        assert_eq!(u32::from_ne_bytes(destroy[..4].try_into().unwrap()), 7);
        assert_eq!(u32::from_ne_bytes(destroy[4..].try_into().unwrap()), (8 << 16) | 1);
        peer.set_nonblocking(true).unwrap();
        let result = peer.read(&mut destroy);
        assert!(matches!(result, Err(e) if e.kind() == io::ErrorKind::WouldBlock),
                "a duplicate cancelled event must not destroy the same source twice");
        s.dispatch(DISPLAY, 1, &7u32.to_ne_bytes()).unwrap();
        assert!(!s.objs.contains_key(&7));
    }

    #[test]
    fn queued_send_after_cancellation_closes_its_fd() {
        let (mut s, _peer) = source_session();
        s.dispatch(7, 1, &[]).unwrap();
        let (mut reader, writer) = UnixStream::pair().unwrap();
        s.wl.fds.push_back(writer.into());
        let mut mime = ((MIME_TEXT.len() + 1) as u32).to_ne_bytes().to_vec();
        mime.extend_from_slice(MIME_TEXT.as_bytes());
        mime.push(0);
        while mime.len() % 4 != 0 { mime.push(0); }
        s.dispatch(7, 0, &mime).unwrap();
        assert!(s.wl.fds.is_empty());
        reader.set_nonblocking(true).unwrap();
        assert_eq!(reader.read(&mut [0u8; 1]).unwrap(), 0);
    }
}
