//! Serialized host requests keep disk I/O and systemctl off the port's serve loop.
use std::io::{self, Write};
use std::os::fd::RawFd;
use std::os::unix::{fs::OpenOptionsExt, process::CommandExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc;

use crate::{codec, port::Port};

pub struct Requests {
    tx: mpsc::Sender<(String, String)>,
    replies: mpsc::Receiver<String>,
    wake_r: RawFd,
}

impl Requests {
    pub fn new() -> Self {
        let (tx, requests) = mpsc::channel::<(String, String)>();
        let (reply, replies) = mpsc::channel();
        let mut fds = [-1; 2];
        if unsafe { libc::pipe2(fds.as_mut_ptr(), libc::O_NONBLOCK | libc::O_CLOEXEC) } < 0 {
            fds = [-1; 2];
        }
        let wake = fds[1];
        std::thread::spawn(move || {
            for (verb, arg) in requests {
                let result = match verb.as_str() {
                    "profiles" => apply(&arg).map(Some),
                    "launch" if valid_appid(&arg) => detached("steam", &[&format!("steam://rungameid/{arg}")]).map(|_| None),
                    "restart-steam" if arg.is_empty() => detached("systemctl", &["--user", "restart", "steam.service"]).map(|_| None),
                    _ => Err(io::Error::new(io::ErrorKind::InvalidInput, "bad request")),
                };
                match result {
                    Ok(Some(line)) => {
                        let _ = reply.send(line);
                        unsafe { libc::write(wake, b"x".as_ptr().cast(), 1) };
                    }
                    Ok(None) => {}
                    Err(error) => eprintln!("fx-progress: {verb}: {error}"),
                }
            }
            unsafe { libc::close(wake) };
        });
        Self { tx, replies, wake_r: fds[0] }
    }

    pub fn request(&self, verb: &str, arg: &str) {
        let _ = self.tx.send((verb.into(), arg.into()));
    }

    pub fn wake_fd(&self) -> RawFd { self.wake_r }

    pub fn pump(&self, port: &mut Port) {
        let mut bytes = [0u8; 64];
        while unsafe { libc::read(self.wake_r, bytes.as_mut_ptr().cast(), bytes.len()) } > 0 {}
        for reply in self.replies.try_iter() { port.send(&reply); }
    }
}

impl Drop for Requests {
    fn drop(&mut self) { unsafe { libc::close(self.wake_r) }; }
}

fn valid_appid(appid: &str) -> bool {
    !appid.is_empty() && appid.bytes().all(|b| b.is_ascii_digit())
}

fn valid_env(env: &str) -> bool {
    !env.chars().any(char::is_control) && env.split_whitespace().all(|word| {
        let Some((name, _)) = word.split_once('=') else { return false };
        let mut chars = name.bytes();
        chars.next().is_some_and(|b| b.is_ascii_alphabetic() || b == b'_')
            && chars.all(|b| b.is_ascii_alphanumeric() || b == b'_')
    })
}

fn validate(bytes: &[u8]) -> io::Result<()> {
    let bad = || io::Error::new(io::ErrorKind::InvalidInput, "invalid profile JSON");
    let root: serde_json::Value = serde_json::from_slice(bytes).map_err(|_| bad())?;
    let root = root.as_object().ok_or_else(bad)?;
    if root.len() != 2 || root.get("v").and_then(|v| v.as_u64()) != Some(1) { return Err(bad()); }
    let games = root.get("games").and_then(|v| v.as_object()).ok_or_else(bad)?;
    for (appid, profile) in games {
        if !valid_appid(appid) { return Err(bad()); }
        let profile = profile.as_object().ok_or_else(bad)?;
        for (key, value) in profile {
            match (key.as_str(), value.as_str()) {
                ("gfx", Some("dxmt" | "mullion" | "vulkan")) => {}
                ("env", Some(env)) if valid_env(env) => {}
                _ => return Err(bad()),
            }
        }
    }
    Ok(())
}

fn runtime() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR").map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() }))).join("steamac")
}

fn atomic_write(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let parent = path.parent().unwrap();
    std::fs::create_dir_all(parent)?;
    let tmp = parent.join(format!(".game-profiles-{}.tmp", std::process::id()));
    let result = (|| {
        let mut file = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&tmp)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        std::fs::rename(&tmp, path)
    })();
    let _ = std::fs::remove_file(tmp);
    result
}

fn status(sha: &str, applied: &str, active: bool) -> &'static str {
    if active && sha != applied.trim() { "pending" } else { "applied" }
}

fn apply(encoded: &str) -> io::Result<String> {
    let bytes = codec::unbase64(encoded).ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid base64"))?;
    validate(&bytes)?;
    let home = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("/home/steamos"));
    persist(&bytes, &home, &runtime())?;
    let sha = codec::sha256_hex(&bytes);
    let active = Command::new("systemctl").args(["--user", "is-active", "--quiet", "steam.service"])
        .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success());
    let applied = std::fs::read_to_string(runtime().join("profiles-applied.sha256")).unwrap_or_default();
    Ok(format!("profiles-ok {sha} {}", status(&sha, &applied, active)))
}

fn persist(bytes: &[u8], home: &Path, runtime: &Path) -> io::Result<()> {
    atomic_write(&home.join(".config/steamac/game-profiles.json"), bytes)?;
    std::fs::create_dir_all(runtime)?;
    std::fs::write(runtime.join("profiles-synced"), b"")
}

fn detached(program: &str, args: &[&str]) -> io::Result<()> {
    // Inherit the session manager's DISPLAY and runtime directory, never a shell.
    let mut command = Command::new(program);
    command.args(args).env("XDG_RUNTIME_DIR", runtime().parent().unwrap())
        .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    unsafe { command.pre_exec(|| {
        if libc::setsid() < 0 { return Err(io::Error::last_os_error()); }
        Ok(())
    }); }
    let mut child = command.spawn()?;
    // Reap without waiting for Steam (or the restart job) in the request worker.
    std::thread::spawn(move || { let _ = child.wait(); });
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn profile_shape_and_words() {
        for valid in [r#"{"v":1,"games":{}}"#, r#"{"v":1,"games":{"570":{"gfx":"dxmt","env":"FOO=1 BAR=;"},"12":{}}}"#, r#"{"v":1,"games":{"570":{"gfx":"mullion"}}}"#] {
            validate(valid.as_bytes()).unwrap();
        }
        for bad in [r#"{"v":2,"games":{}}"#, r#"{"v":1,"games":[]}"#, r#"{"v":1,"games":{"../1":{}}}"#, r#"{"v":1,"games":{"1":{"gfx":"default"}}}"#, r#"{"v":1,"games":{"1":{"driver":"moltenvk"}}}"#, r#"{"v":1,"games":{"1":{"env":"FOO=1 command"}}}"#, r#"{"v":1,"games":{"1":{"env":"1BAD=x"}}}"#, r#"{"v":1,"games":{"1":{"env":"FOO=1\nBAR=2"}}}"#] {
            assert!(validate(bad.as_bytes()).is_err(), "{bad}");
        }
        for bad in ["", "-1", "12/3", "１２", "1 2"] { assert!(!valid_appid(bad)); }
    }

    #[test]
    fn pending_only_for_active_different_set() {
        assert_eq!(status("abc", "abc\n", true), "applied");
        assert_eq!(status("abc", "", true), "pending");
        assert_eq!(status("abc", "def", false), "applied");
    }

    #[test]
    fn persisted_set_is_exact_and_sync_is_boot_local() {
        let dir = std::env::temp_dir().join(format!("fx-profiles-{}", std::process::id()));
        let home = dir.join("home");
        let runtime = dir.join("runtime/steamac");
        let path = home.join(".config/steamac/game-profiles.json");
        let bytes = b"{ \"v\": 1, \"games\": {} }";
        validate(bytes).unwrap();
        persist(bytes, &home, &runtime).unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), bytes);
        assert!(runtime.join("profiles-synced").is_file());
        persist(b"{\"v\":1,\"games\":{}}", &home, &runtime).unwrap();
        assert_eq!(std::fs::read(&path).unwrap(), b"{\"v\":1,\"games\":{}}");
        assert_eq!(std::fs::read_dir(path.parent().unwrap()).unwrap().count(), 1);
        assert!(!runtime.join("profiles-applied.sha256").exists());
        std::fs::remove_dir_all(dir).unwrap();
    }
}
