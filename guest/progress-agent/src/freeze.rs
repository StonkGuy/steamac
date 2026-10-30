//! Game pause while the launcher is in the background (Settings > General
//! "Pause the game"): host -> guest `freeze-game <appid>` freezes the game's
//! cgroup with the cgroup v2 freezer, `thaw-game` thaws it. Only the game:
//! the Steam client (downloads, updates) keeps running.
//!
//! The game's cgroups are the user manager's scopes Steam starts it in,
//! `app-steam-app<appid>-<n>.scope` (steam-launch-wrapper, anywhere below
//! user@<uid>.service). They are frozen through the user manager
//! (`systemctl --user freeze`, systemd >= 246; its FreezerState stays right),
//! falling back to writing the scope's `cgroup.freeze` (the session user owns
//! its delegated subtree) if systemctl fails or takes more than 2 s.
//!
//! Never leaves a game frozen: while frozen the host sends `still-background`
//! every 2 s; without one for KEEPALIVE_TIMEOUT the agent thaws by itself (the
//! launcher died / was killed). Also thawed when the focus leaves the game,
//! on agent exit (Drop) and before a new freeze.
//!
//! Replies (queued, sent by the main loop with `send_replies`): `game-frozen
//! <appid>` once a freeze took effect, `game-thawed <appid>` after any thaw
//! (host request, keepalive timeout, focus change, agent exit), so the
//! launcher's "Game paused" overlay shows the real state.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use crate::port::Port;

pub const KEEPALIVE_TIMEOUT: Duration = Duration::from_secs(6);

pub struct Freezer {
    appid: Option<u32>,
    frozen: Vec<PathBuf>,
    last_keepalive: Instant,
    replies: Vec<String>,
}

impl Freezer {
    pub fn new() -> Freezer {
        Freezer { appid: None, frozen: Vec::new(), last_keepalive: Instant::now(), replies: Vec::new() }
    }

    pub fn freeze(&mut self, appid: u32) {
        if self.appid == Some(appid) {
            self.last_keepalive = Instant::now();
            return;
        }
        self.thaw("new freeze request");
        let Some(root) = user_manager_cgroup() else {
            eprintln!("fx-progress: freeze {appid}: user manager cgroup not found");
            return;
        };
        let mut games = game_scopes(&root).unwrap_or_default();
        let scopes = games.remove(&appid).unwrap_or_default();
        if scopes.is_empty() {
            eprintln!("fx-progress: freeze {appid}: no app-steam-app{appid}-*.scope below {}", root.display());
            return;
        }
        let mut any = false;
        for s in &scopes {
            any |= set_frozen(s, true);
        }
        // Remember the scopes even if a write failed: thaw them all later.
        self.appid = Some(appid);
        self.frozen = scopes;
        self.last_keepalive = Instant::now();
        if any {
            self.replies.push(format!("game-frozen {appid}"));
        }
    }

    pub fn keepalive(&mut self) {
        self.last_keepalive = Instant::now();
    }

    pub fn thaw(&mut self, why: &str) {
        let Some(appid) = self.appid.take() else { return };
        for s in self.frozen.drain(..) {
            // A scope whose game exited is gone: nothing to thaw.
            if s.exists() {
                set_frozen(&s, false);
            }
        }
        eprintln!("fx-progress: thawed game {appid} ({why})");
        self.replies.push(format!("game-thawed {appid}"));
    }

    /// Thaw when the host stopped sending keepalives, or the focus left the game.
    pub fn pump(&mut self, focused_game: Option<u32>) {
        let Some(appid) = self.appid else { return };
        if self.last_keepalive.elapsed() > KEEPALIVE_TIMEOUT {
            self.thaw("no keepalive from the launcher");
        } else if focused_game != Some(appid) {
            self.thaw("focus left the game");
        }
    }

    /// Queue the pending `game-frozen` / `game-thawed` replies on the port.
    pub fn send_replies(&mut self, port: &mut Port) {
        for r in self.replies.drain(..) {
            port.send(&r);
        }
    }

    /// Longest ppoll wait while frozen (keepalive deadline check).
    pub fn wait_ms(&self) -> Option<i64> {
        self.appid.map(|_| 1000)
    }
}

impl Drop for Freezer {
    fn drop(&mut self) {
        self.thaw("agent exiting");
    }
}

/// Freeze / thaw one scope: `systemctl --user freeze|thaw <unit>` (bounded to
/// 2 s), else its cgroup.freeze directly. True if either took effect.
fn set_frozen(scope: &Path, frozen: bool) -> bool {
    let verb = if frozen { "freeze" } else { "thaw" };
    let unit = scope.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let via_systemd = std::process::Command::new("systemctl")
        .args(["--user", verb, &unit])
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .ok()
        .and_then(|mut child| {
            let deadline = Instant::now() + Duration::from_secs(2);
            loop {
                match child.try_wait() {
                    Ok(Some(status)) => return Some(status.success()),
                    Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
                    _ => {
                        let _ = child.kill();
                        let _ = child.wait();
                        return None;
                    }
                }
            }
        })
        == Some(true);
    if via_systemd {
        eprintln!("fx-progress: {verb} {unit} (systemctl)");
        return true;
    }
    match std::fs::write(scope.join("cgroup.freeze"), if frozen { "1" } else { "0" }) {
        Ok(()) => {
            eprintln!("fx-progress: {verb} {unit} (cgroup.freeze)");
            true
        }
        Err(e) => {
            eprintln!("fx-progress: {verb} {}: {e}", scope.display());
            false
        }
    }
}

/// /sys/fs/cgroup/.../user@<uid>.service of this process (cgroup v2).
pub(crate) fn user_manager_cgroup() -> Option<PathBuf> {
    if let Some(root) = std::env::var_os("FX_PROGRESS_CGROUP_ROOT") {
        return Some(PathBuf::from(root));
    }
    let own = std::fs::read_to_string("/proc/self/cgroup").ok()?;
    let path = own.lines().find_map(|l| l.strip_prefix("0::"))?;
    let mut acc = PathBuf::from("/sys/fs/cgroup");
    for part in path.trim_start_matches('/').split('/') {
        acc.push(part);
        if part.starts_with("user@") && part.ends_with(".service") {
            return Some(acc);
        }
    }
    None
}

/// Steam scopes grouped by app id, shared by freezing and session statistics.
/// A failed root read is not an empty scan: callers must not infer game exits.
pub(crate) fn game_scopes(root: &Path) -> std::io::Result<BTreeMap<u32, Vec<PathBuf>>> {
    let mut out = BTreeMap::new();
    find_scopes(std::fs::read_dir(root)?, 0, &mut out);
    Ok(out)
}

fn find_scopes(entries: std::fs::ReadDir, depth: usize, out: &mut BTreeMap<u32, Vec<PathBuf>>) {
    for e in entries.flatten() {
        if !e.file_type().map_or(false, |t| t.is_dir()) {
            continue;
        }
        let name = e.file_name();
        let name = name.to_string_lossy();
        let appid = name.strip_prefix("app-steam-app")
            .and_then(|s| s.strip_suffix(".scope"))
            .and_then(|s| s.split_once('-'))
            .and_then(|(id, suffix)| {
                if !suffix.is_empty() && !id.is_empty() && id.bytes().all(|b| b.is_ascii_digit()) {
                    id.parse::<u32>().ok()
                } else {
                    None
                }
            });
        if let Some(appid) = appid {
            out.entry(appid).or_default().push(e.path());
        } else if depth < 4 && (name.ends_with(".slice") || name.ends_with(".service")) {
            if let Ok(children) = std::fs::read_dir(e.path()) {
                find_scopes(children, depth + 1, out);
            }
        }
    }
}
