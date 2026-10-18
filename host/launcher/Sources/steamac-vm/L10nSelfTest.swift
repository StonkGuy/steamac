import Foundation

/// --selftest-l10n: the localizations this binary would show (its bundle's <lang>.lproj, compiled from
/// Localizable.xcstrings by l10n.py) are complete. Every language has a table, every key one language
/// translates the others translate too, with the same format arguments, and the UI strings in `probes`
/// resolve to a translation. A build without the tables (no l10n.py compile) fails: it shows English
/// to everyone. The source-side check (every string the compiler extracted is in the catalog and
/// translated) is `l10n.py check --strict --stringsdata DIR`, run by build.sh.
enum L10nSelfTest {
    static let languages = ["ru", "zh-Hans"]

    /// Strings of the launcher's own settings and windows (graphics path, render scale, frame latency,
    /// GPU-idle card) whose translation must be present in every language.
    static var probes: [String] {
        ["Graphics path", "DXMT (lean)", "Vulkan (classic)", "Applies from the next start.",
         "Not available here: %@; Vulkan is used.", "Render scale", "Off (1.0×)", "Upscale lower game resolutions",
         "Frame latency", "Loading…", "not included in this build (%@)"]
    }

    static func run() -> Never {
        var failures = 0
        func check(_ ok: Bool, _ what: String) { if !ok { failures += 1; log("selftest-l10n: FAIL \(what)") } }
        var tables: [String: [String: String]] = [:]
        for lang in languages {
            var t: [String: String] = [:]
            if let p = Bundle.main.path(forResource: "Localizable", ofType: "strings", inDirectory: nil, forLocalization: lang),
               let d = NSDictionary(contentsOfFile: p) as? [String: String] {
                t = d
            }
            // Plural strings: the key's format is the value's NSStringLocalizedFormatKey.
            if let p = Bundle.main.path(forResource: "Localizable", ofType: "stringsdict", inDirectory: nil, forLocalization: lang),
               let d = NSDictionary(contentsOfFile: p) as? [String: Any] {
                for (k, v) in d { t[k] = ((v as? [String: Any])?["NSStringLocalizedFormatKey"] as? String) ?? "" }
            }
            check(!t.isEmpty, "\(lang): no Localizable table in \(Bundle.main.bundlePath) (built without l10n.py compile?)")
            tables[lang] = t
        }
        let all = Set(tables.values.flatMap(\.keys))
        for lang in languages {
            let t = tables[lang] ?? [:]
            for key in all.sorted() where t[key] == nil { check(false, "\(lang) missing: \(key)") }
            for key in probes where t[key] == nil || t[key] == key { check(false, "\(lang) untranslated: \(key)") }
            for (key, value) in t where !value.contains("%#@") && formatArguments(value) != formatArguments(key) {
                check(false, "\(lang) format arguments differ: \(key) -> \(value)")
            }
        }
        log("selftest-l10n: \(all.count) keys, \(languages.joined(separator: ", "))")
        log(failures == 0 ? "selftest-l10n: PASS" : "selftest-l10n: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }

    /// Conversions of a printf-style format by position (%1$@ and %@ alike), %% ignored; "lld" and "d" are both "d".
    static func formatArguments(_ s: String) -> [Int: Character] {
        var out: [Int: Character] = [:], seq = 0
        let re = try! NSRegularExpression(pattern: #"%(?:(\d+)\$)?[-+ #0']*\d*(?:\.\d+)?(?:hh|h|ll|l|q|z|t|j|L)?([@dDiuUxXoOfFeEgGaAcCsSp%])"#)
        for m in re.matches(in: s, range: NSRange(s.startIndex..., in: s)) {
            let conv = Character(String(s[Range(m.range(at: 2), in: s)!]).lowercased())
            if conv == "%" { continue }
            seq += 1
            let pos = Range(m.range(at: 1), in: s).flatMap { Int(s[$0]) } ?? seq
            out[pos] = "diu".contains(conv) ? "d" : ("feg".contains(conv) ? "f" : conv)
        }
        return out
    }
}
