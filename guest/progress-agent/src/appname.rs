//! Steam app name lookup for `game <appid> <name>` (launcher settings list).
//!
//! Library folders: ~/.local/share/Steam/steamapps/libraryfolders.vdf lists
//! every library ("path" "<dir>"); each has steamapps/appmanifest_<id>.acf
//! whose AppState "name" key is the display name. Both files are Valve KeyValues
//! text; only quoted tokens matter here, so a tiny tokenizer is enough.

use std::path::PathBuf;

/// Quoted tokens of a KeyValues text, with \" \\ \n \t unescaped.
fn tokens(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        if c != '"' {
            continue;
        }
        let mut s = String::new();
        while let Some(c) = chars.next() {
            match c {
                '"' => break,
                '\\' => match chars.next() {
                    Some('n') => s.push('\n'),
                    Some('t') => s.push('\t'),
                    Some(o) => s.push(o),
                    None => break,
                },
                o => s.push(o),
            }
        }
        out.push(s);
    }
    out
}

/// Value of the first `"key" "value"` pair (case-insensitive key).
fn first_value(text: &str, key: &str) -> Option<String> {
    let t = tokens(text);
    t.windows(2).find(|w| w[0].eq_ignore_ascii_case(key)).map(|w| w[1].clone())
}

fn library_dirs(steam_root: &PathBuf) -> Vec<PathBuf> {
    let mut dirs = vec![steam_root.clone()];
    if let Ok(vdf) = std::fs::read_to_string(steam_root.join("steamapps/libraryfolders.vdf")) {
        let t = tokens(&vdf);
        for w in t.windows(2) {
            if w[0].eq_ignore_ascii_case("path") {
                let p = PathBuf::from(&w[1]);
                if !dirs.contains(&p) {
                    dirs.push(p);
                }
            }
        }
    }
    dirs
}

/// Display name of `appid` from the first library that has its manifest,
/// sanitised to one line of printable text; None if not installed/unknown.
fn steam_root() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/home/steamos".into());
    std::env::var("FX_PROGRESS_STEAM_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(format!("{home}/.local/share/Steam")))
}

/// Known VR titles (installed copies are listed even without a .vrmanifest).
const KNOWN_VR: &[u32] = &[438100 /* VRChat */];
/// VR companions/tools, not games: never offered as a game to launch.
const NOT_GAMES: &[u32] = &[2370570 /* Standable */, 250820 /* SteamVR */, 228980 /* Steamworks Common Redistributables */];

/// Installed, fully-downloaded VR apps across all library folders: known VR
/// ids, plus any app whose install dir has a top-level `*.vrmanifest`
/// (the OpenVR app manifest VR titles ship). Sorted, deduplicated.
pub fn vr_apps_in(root: &PathBuf) -> Vec<u32> {
    let mut out = Vec::new();
    for dir in library_dirs(root) {
        let apps = dir.join("steamapps");
        let Ok(rd) = std::fs::read_dir(&apps) else { continue };
        for e in rd.flatten() {
            let name = e.file_name().to_string_lossy().into_owned();
            let Some(id) = name.strip_prefix("appmanifest_").and_then(|n| n.strip_suffix(".acf")).and_then(|n| n.parse::<u32>().ok()) else { continue };
            if NOT_GAMES.contains(&id) || out.contains(&id) {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(e.path()) else { continue };
            // StateFlags 4 = fully installed.
            if first_value(&text, "StateFlags").and_then(|v| v.parse::<u32>().ok()).map_or(true, |f| f & 4 == 0) {
                continue;
            }
            let vr = KNOWN_VR.contains(&id)
                || first_value(&text, "installdir").is_some_and(|d| {
                    std::fs::read_dir(apps.join("common").join(d)).map_or(false, |rd| {
                        rd.flatten().any(|f| f.file_name().to_string_lossy().ends_with(".vrmanifest"))
                    })
                });
            if vr {
                out.push(id);
            }
        }
    }
    out.sort_unstable();
    out
}

/// `vr-apps <id> ...` reply line for the host (`vr-apps` alone when none).
pub fn vr_apps_line() -> String {
    let ids = vr_apps_in(&steam_root());
    std::iter::once("vr-apps".to_string()).chain(ids.iter().map(|i| i.to_string())).collect::<Vec<_>>().join(" ")
}

pub fn lookup(appid: u32) -> Option<String> {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/home/steamos".into());
    let root = std::env::var("FX_PROGRESS_STEAM_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(format!("{home}/.local/share/Steam")));
    for dir in library_dirs(&root) {
        let acf = dir.join(format!("steamapps/appmanifest_{appid}.acf"));
        let Ok(text) = std::fs::read_to_string(&acf) else { continue };
        if let Some(name) = first_value(&text, "name") {
            let clean: String = name
                .chars()
                .map(|c| if c.is_control() { ' ' } else { c })
                .collect::<String>()
                .split_whitespace()
                .collect::<Vec<_>>()
                .join(" ");
            if !clean.is_empty() {
                return Some(clean);
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn acf_name() {
        let acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"570\"\n\t\"name\"\t\t\"Dota \\\"2\\\"\"\n\t\"UserConfig\"\n\t{\n\t\t\"name\"\t\"x\"\n\t}\n}\n";
        assert_eq!(first_value(acf, "name").as_deref(), Some("Dota \"2\""));
    }

    #[test]
    fn vdf_paths() {
        let vdf = "\"libraryfolders\"\n{\n\"0\"\n{\n\"path\"\t\t\"/home/steamos/.local/share/Steam\"\n}\n\"1\"\n{\n\"path\"\t\t\"/run/media/sd\"\n}\n}";
        let t = tokens(vdf);
        let paths: Vec<_> = t.windows(2).filter(|w| w[0] == "path").map(|w| w[1].clone()).collect();
        assert_eq!(paths, vec!["/home/steamos/.local/share/Steam", "/run/media/sd"]);
    }

    #[test]
    fn vr_apps_fixture() {
        let base = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("testdata/vrlib");
        let tmp = std::env::temp_dir().join(format!("fx-vrlib-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&tmp);
        // Library 0 = root; libraryfolders.vdf points at library 1 (path rewritten to tmp).
        let copy = |from: &str, to: PathBuf| {
            std::fs::create_dir_all(to.parent().unwrap()).unwrap();
            std::fs::copy(base.join(from), to).unwrap();
        };
        let root = tmp.join("root");
        let lib1 = tmp.join("lib1");
        let vdf = std::fs::read_to_string(base.join("libraryfolders.vdf")).unwrap().replace("@LIB1@", lib1.to_str().unwrap());
        std::fs::create_dir_all(root.join("steamapps")).unwrap();
        std::fs::write(root.join("steamapps/libraryfolders.vdf"), vdf).unwrap();
        for f in ["appmanifest_438100.acf", "appmanifest_2370570.acf", "appmanifest_570.acf", "appmanifest_999.acf"] {
            copy(f, root.join("steamapps").join(f));
        }
        copy("appmanifest_1000.acf", lib1.join("steamapps/appmanifest_1000.acf"));
        copy("bench.vrmanifest", lib1.join("steamapps/common/Bench VR/bench.vrmanifest"));
        assert_eq!(vr_apps_in(&root), vec![1000, 438100]);
        let _ = std::fs::remove_dir_all(&tmp);
    }
}
