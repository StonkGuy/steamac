//! `launch-vr <appid>` from the host (Settings > VR "Start in VR", or the headset's WiVRn app list relayed by
//! the launcher): asks the running Steam client to start the game in its VR launch mode,
//! `steam -ifrunning steam://launch/<appid>/vr` (a plain `steam://rungameid/<appid>` picks the game's default,
//! for VRChat the desktop mode); not again while the game runs. Fire and forget: Steam shows its own errors; the child is reaped on a thread.

use std::process::{Command, Stdio};

/// The Steam URL for `id` in VR mode.
pub fn url(id: u32) -> String {
    format!("steam://launch/{id}/vr")
}

/// Parses the request argument: a Steam app id (decimal, non-zero).
pub fn parse(arg: &str) -> Option<u32> {
    arg.parse::<u32>().ok().filter(|&id| id != 0)
}

/// A game is running when a process carries Steam's `AppId=<id>` launch argument (the reaper's
/// `SteamLaunch AppId=<id> -- ...`, which lives as long as the game).
pub fn running(id: u32) -> bool {
    let Ok(dir) = std::fs::read_dir("/proc") else { return false };
    dir.flatten().any(|e| {
        let name = e.file_name();
        name.to_str().is_some_and(|n| n.bytes().all(|b| b.is_ascii_digit()))
            && std::fs::read(e.path().join("cmdline")).is_ok_and(|c| has_app_id(&c, id))
    })
}

/// `cmdline` (NUL-separated) holds the argument `AppId=<id>`.
pub fn has_app_id(cmdline: &[u8], id: u32) -> bool {
    let want = format!("AppId={id}");
    cmdline.split(|&b| b == 0).any(|a| a == want.as_bytes())
}

pub fn launch(id: u32) {
    // the headset's app list can ask again (after a reconnect, a second tap): never a second instance
    if running(id) {
        eprintln!("fx-progress: launch-vr {id}: already running, not started again");
        return;
    }
    let url = url(id);
    match Command::new("steam")
        .args(["-ifrunning", &url])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(mut child) => {
            eprintln!("fx-progress: launch-vr {id}: steam -ifrunning {url}");
            std::thread::spawn(move || {
                let _ = child.wait();
            });
        }
        Err(e) => eprintln!("fx-progress: launch-vr {id}: cannot run steam: {e}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vr_url_and_ids() {
        assert_eq!(url(438100), "steam://launch/438100/vr");
        assert_eq!(parse("438100"), Some(438100));
        assert_eq!(parse("0"), None);
        assert_eq!(parse("-1"), None);
        assert_eq!(parse("438100 x"), None);
        assert_eq!(parse(""), None);
    }

    #[test]
    fn running_game_detection() {
        let reaper = b"/home/steamos/.local/share/Steam/steamrtarm64/reaper\0SteamLaunch\0AppId=438100\0--\0launch.exe\0";
        assert!(has_app_id(reaper, 438100));
        assert!(!has_app_id(reaper, 43810));
        assert!(!has_app_id(b"steam\0-ifrunning\0steam://launch/438100/vr\0", 438100));
    }
}
