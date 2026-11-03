//! `keyboard-layout <active index> <layout[(variant)]>...` from the launcher: the Mac's
//! keyboard layouts for Desktop Mode. /usr/lib/steamac/desktop-keyboard applies them to the
//! nested Plasma session (kxkbrc, KWin over D-Bus) and does nothing without one; the launcher
//! sends them on every layout change on the Mac and whenever Desktop Mode gets the focus.
//!
//! One worker thread runs the helper, one request at a time and only the latest one: a burst
//! of switches on the Mac ends on the layout selected last, and the agent never waits.

use std::process::Command;
use std::sync::mpsc::{channel, Sender};
use std::sync::{Mutex, OnceLock};

const HELPER: &str = "/usr/lib/steamac/desktop-keyboard";

static WORKER: OnceLock<Mutex<Sender<Vec<String>>>> = OnceLock::new();

/// Words after `keyboard-layout`, checked enough that the helper gets plain arguments.
fn parse(arg: &str) -> Option<Vec<String>> {
    let words: Vec<String> = arg.split_whitespace().map(str::to_owned).collect();
    let ok = words.len() >= 2
        && words[0].parse::<usize>().is_ok_and(|i| i < words.len() - 1)
        && words[1..].iter().all(|w| w.len() <= 40 && w.bytes().all(|b| b.is_ascii_alphanumeric() || b"_()-".contains(&b)));
    ok.then_some(words)
}

pub fn request(arg: &str) {
    let Some(words) = parse(arg) else {
        eprintln!("fx-progress: keyboard-layout: bad request {arg:?}");
        return;
    };
    let tx = WORKER.get_or_init(|| {
        let (tx, rx) = channel::<Vec<String>>();
        std::thread::spawn(move || {
            while let Ok(mut words) = rx.recv() {
                while let Ok(newer) = rx.try_recv() {
                    words = newer;
                }
                match Command::new(HELPER).args(&words).status() {
                    Ok(s) if s.success() => {}
                    Ok(s) => eprintln!("fx-progress: {HELPER} {}: {s}", words.join(" ")),
                    Err(e) => eprintln!("fx-progress: {HELPER}: {e}"),
                }
            }
        });
        Mutex::new(tx)
    });
    let _ = tx.lock().map(|tx| tx.send(words));
}

#[cfg(test)]
mod tests {
    use super::parse;

    #[test]
    fn requests() {
        assert_eq!(parse("1 us ru(mac)"), Some(vec!["1".into(), "us".into(), "ru(mac)".into()]));
        assert_eq!(parse("0 us"), Some(vec!["0".into(), "us".into()]));
        assert_eq!(parse("1 us(dvorak-l) ca(fr-legacy)"), Some(vec!["1".into(), "us(dvorak-l)".into(), "ca(fr-legacy)".into()]));
        assert_eq!(parse("2 us ru"), None, "index beyond the list");
        assert_eq!(parse("us ru"), None, "no index");
        assert_eq!(parse("0"), None, "no layouts");
        assert_eq!(parse("0 us;rm"), None, "shell characters");
    }
}
