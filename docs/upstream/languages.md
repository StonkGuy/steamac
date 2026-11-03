# Languages

The launcher's interface is in English, Russian and Simplified Chinese and follows the macOS language
(System Settings → General → Language & Region, also per app under Applications). Logs, command-line
output, crash reports and problem reports stay in English. The fixed Steam stage texts of the guest
agent ("Checking for Steam updates", …) are shown translated (`BootProgress.localizedGuestText`); other
guest text (systemd lines, SteamOS itself) is not translated by the launcher.

Strings live in String Catalogs: `host/launcher/Localizable.xcstrings` (keys are the English text) and
`host/launcher/InfoPlist.xcstrings` (permission prompts). In code, SwiftUI literals (`Text("…")`,
`Button("…")`) are localized automatically; every other UI string is written as
`String(localized: "… \(value) …")` — one sentence per key, arguments interpolated, never assembled
from fragments — and text for logs or the CLI stays a plain English string. `host/launcher/build.sh`
compiles with `-emit-localized-strings`, syncs the extracted keys into the catalog (new keys are
added, removed ones marked stale — commit the catalog with the code), prints untranslated keys and
format-argument mismatches (`host/launcher/l10n.py check`), and compiles the catalogs into
`<lang>.lproj` next to `work/out/steamac-vm` and in the app's `Contents/Resources`. `dist.sh` refuses
a release with an incomplete language. Translating outside Xcode: `host/launcher/l10n.py export ru
todo.json` lists what is missing, `host/launcher/l10n.py import ru done.json` adds
`{"key": "text"}` or `{"key": {"plural": {"one": …, "few": …, "many": …, "other": …}}}`; Xcode can
also open the catalogs directly. To check a language without changing the system one:
`work/out/steamac-vm -AppleLanguages '(ru)' …`.
