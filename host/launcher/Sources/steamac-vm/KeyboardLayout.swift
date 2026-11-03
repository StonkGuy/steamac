import AppKit
import Carbon
import Combine
import Foundation

/// Settings > General "Use the Mac's keyboard layouts in Desktop Mode": the Mac's enabled
/// keyboard layouts (in macOS's order) and the one selected now go to the progress agent as
/// XKB layouts, `keyboard-layout <active index> <layout[(variant)]>...`, which has the nested
/// Plasma session use them (/usr/lib/steamac/desktop-keyboard: kxkbrc, KWin over D-Bus). Sent
/// on every input source change on the Mac and whenever Desktop Mode gets the focus (Plasma
/// starts without them). The keys stay the Mac's physical keys either way (evdev codes); the
/// layout decides what they type. Main thread only.
///
/// The change notification comes late: switching with the Globe key, macOS often posts
/// kTISNotifySelectedKeyboardInputSourceChanged 0.55-0.85 s (up to 2.6 s) after the input
/// source changed. Poll only while enabled, Desktop Mode focused and the launcher active;
/// the notification covers the rest.
final class KeyboardLayoutSync {
    private let settings: LauncherSettings
    private let send: (String) -> Bool
    private var last: String?
    private var observers: [NSObjectProtocol] = []
    private var subscription: AnyCancellable?
    private var poll: Timer?
    private var currentID = ""
    private var focus: GuestFocus = .steam

    init(settings: LauncherSettings, send: @escaping (String) -> Bool) {
        self.settings = settings
        self.send = send
    }

    func start() {
        let dnc = DistributedNotificationCenter.default()
        for name in [kTISNotifySelectedKeyboardInputSourceChanged, kTISNotifyEnabledKeyboardInputSourcesChanged].compactMap({ $0 }) {
            observers.append(dnc.addObserver(forName: Notification.Name(name as String), object: nil, queue: .main) { [weak self] _ in
                self?.push(force: false)
            })
        }
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.updatePolling()
        })
        observers.append(nc.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            self?.updatePolling()
        })
        updatePolling()
        // @Published emits before the property changes.
        subscription = settings.$followMacKeyboard.dropFirst().receive(on: DispatchQueue.main).sink { [weak self] _ in
            self?.updatePolling()
            self?.push(force: true)
        }
    }

    /// Every guest focus change reaches here, including leaving Desktop Mode.
    func focused(_ focus: GuestFocus) {
        self.focus = focus
        updatePolling()
        guard focus == .desktop else { return }
        push(force: true)
        // Plasma can select its first layout after KWin accepts the initial request.
        for delay in [1.5, 4, 8] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.push(force: true) }
        }
    }

    private func updatePolling() {
        let on = settings.followMacKeyboard && focus == .desktop && NSApp.isActive
        guard on != (poll != nil) else { return }
        poll?.invalidate()
        poll = nil
        guard on else { return }
        currentID = KeyboardLayoutSync.currentID()
        let t = Timer(timeInterval: 0.03, repeats: true) { [weak self] _ in
            guard let self else { return }
            let id = KeyboardLayoutSync.currentID()
            if id != self.currentID {
                self.currentID = id
                self.push(force: false)
            }
        }
        t.tolerance = 0.01
        RunLoop.main.add(t, forMode: .common)
        poll = t
    }

    private static func currentID() -> String {
        guard let s = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue() else { return "" }
        return property(s, kTISPropertyInputSourceID) as? String ?? ""
    }

    private func push(force: Bool) {
        guard settings.followMacKeyboard, focus == .desktop,
              let line = KeyboardLayoutSync.request() else { return }
        guard force || line != last else { return }
        if send(line) {
            if line != last { log("keyboard: \(line)") }
            last = line
        }
    }

    /// `keyboard-layout …` for the Mac's layouts now, nil without any.
    static func request() -> String? {
        guard let current = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let active = xkb(for: current) else { return nil }
        return request(specs: enabledLayouts().compactMap { xkb(for: $0) }, active: active)
    }

    /// XKB supports four groups; keep input-menu order, replacing the last if active is later.
    static func request(specs: [String], active: String) -> String {
        var selected: [String] = []
        for spec in specs where !selected.contains(spec) {
            if selected.count < 4 { selected.append(spec) }
        }
        if !selected.contains(active) {
            if selected.count == 4 { selected.removeLast() }
            selected.append(active)
        }
        let index = selected.firstIndex(of: active)!
        return "keyboard-layout \(index) " + selected.joined(separator: " ")
    }

    /// The enabled keyboard layouts (not input methods: their layout is the current one when
    /// they are selected), in the order of the Input menu.
    private static func enabledLayouts() -> [TISInputSource] {
        let filter = [kTISPropertyInputSourceCategory: kTISCategoryKeyboardInputSource,
                      kTISPropertyInputSourceType: kTISTypeKeyboardLayout] as CFDictionary
        guard let list = TISCreateInputSourceList(filter, false)?.takeRetainedValue() as? [TISInputSource] else { return [] }
        return list
    }

    private static func property(_ s: TISInputSource, _ key: CFString) -> AnyObject? {
        guard let p = TISGetInputSourceProperty(s, key) else { return nil }
        return Unmanaged<AnyObject>.fromOpaque(p).takeUnretainedValue()
    }

    static func xkb(for s: TISInputSource) -> String? {
        let id = property(s, kTISPropertyInputSourceID) as? String ?? ""
        let languages = property(s, kTISPropertyInputSourceLanguages) as? [String] ?? []
        return xkb(id: id, languages: languages)
    }

    /// Only explicit names and unambiguous first languages; never guess an XKB country code.
    static func xkb(id: String, languages: [String]) -> String? {
        let name = id.hasPrefix("com.apple.keylayout.") ? String(id.dropFirst("com.apple.keylayout.".count)) : ""
        if let spec = byName[name] { return spec }
        guard let language = languages.first else { return nil }
        return byLanguage[language]
    }

    private static let byName: [String: String] = [
        "US": "us", "ABC": "us", "USExtended": "us", "Australian": "us", "Canadian": "us",
        "USInternational-PC": "us(intl)", "Dvorak": "us(dvorak)", "Colemak": "us(colemak)",
        "DVORAK-QWERTYCMD": "us(dvorak)", "Dvorak-Left": "us(dvorak-l)", "Dvorak-Right": "us(dvorak-r)",
        "Hawaiian": "us",
        "British": "gb(mac)", "British-PC": "gb", "Irish": "ie",
        "Russian": "ru(mac)", "RussianWin": "ru(winkeys)", "Russian-Phonetic": "ru(phonetic_mac)",
        "Ukrainian": "ua(macOS)", "Ukrainian-PC": "ua(winkeys)", "Byelorussian": "by",
        "Ukrainian-QWERTY": "ua(phonetic)",
        "German": "de(mac)", "Austrian": "at(mac)", "SwissGerman": "ch(de_mac)", "SwissFrench": "ch(fr_mac)",
        "French": "fr(mac)", "French-PC": "fr", "Belgian": "be", "Spanish": "es", "Spanish-ISO": "es",
        "Italian": "it(mac)", "Italian-Pro": "it", "Portuguese": "pt(mac)", "Brazilian": "br",
        "Brazilian-ABNT2": "br", "Brazilian-Pro": "br",
        "Canadian-CSA": "ca(multix)", "CanadianFrench-PC": "ca(fr-legacy)",
        "Swedish": "se(mac)", "Swedish-Pro": "se", "Norwegian": "no(mac)", "Danish": "dk(mac)", "Finnish": "fi(mac)",
        "Dutch": "nl(mac)", "Polish": "pl", "PolishPro": "pl", "Czech": "cz", "Czech-QWERTY": "cz(qwerty)",
        "Slovak": "sk", "Slovak-QWERTY": "sk(qwerty)", "Hungarian": "hu", "Hungarian-QWERTY": "hu(qwerty)",
        "Turkish": "tr(f)", "Turkish-Standard": "tr(f)", "Turkish-QWERTY": "tr", "Turkish-QWERTY-PC": "tr", "Greek": "gr",
        "Hebrew": "il", "Hebrew-QWERTY": "il(phonetic)", "Bulgarian-Phonetic": "bg(phonetic)",
        "Arabic-AZERTY": "ara(azerty)", "Serbian-Latin": "rs(latin)", "Sinhala": "lk(sin_phonetic)",
        "Sinhala-QWERTY": "lk(sin_phonetic)", "NorthernSami": "no(smi)",
        "Kazakh": "kz", "Georgian-QWERTY": "ge", "Armenian-HMQWERTY": "am",
    ]

    /// Closed language allowlist, not a general ISO language → XKB country conversion.
    private static let byLanguage: [String: String] = [
        "en": "us", "ru": "ru", "uk": "ua", "be": "by", "kk": "kz", "he": "il", "el": "gr", "cs": "cz", "da": "dk",
        "sv": "se", "ja": "jp", "ko": "kr", "ka": "ge", "hy": "am", "et": "ee", "sl": "si",
        "fa": "ir", "ar": "ara", "vi": "vn", "nb": "no", "nn": "no",
        "de": "de", "fr": "fr", "es": "es", "it": "it", "pl": "pl", "lt": "lt", "lv": "lv",
        "fi": "fi", "is": "is", "hu": "hu", "sk": "sk", "bg": "bg", "ro": "ro", "th": "th",
    ]

    /// SettingsSelfTest: the mapping of a few layouts.
    static func selfCheck() -> [String] {
        var problems: [String] = []
        for (id, languages, want) in [("com.apple.keylayout.ABC", ["en"], "us"),
                                      ("com.apple.keylayout.Russian", ["ru"], "ru(mac)"),
                                      ("com.apple.keylayout.RussianWin", ["ru"], "ru(winkeys)"),
                                      ("com.apple.keylayout.Hawaiian", ["haw", "en"], "us"),
                                      ("com.apple.keylayout.Lithuanian", ["lt"], "lt"),
                                      ("com.apple.keylayout.Ukrainian-QWERTY", ["uk"], "ua(phonetic)")] {
            let got = xkb(id: id, languages: languages)
            if got != want { problems.append("keyboard layout \(id): \(got ?? "nil"), expected \(want)") }
        }
        for (name, want) in [
            ("Spanish", "es"), ("Brazilian-ABNT2", "br"), ("Brazilian-Pro", "br"),
            ("Canadian-CSA", "ca(multix)"), ("CanadianFrench-PC", "ca(fr-legacy)"),
            ("Turkish", "tr(f)"), ("Turkish-Standard", "tr(f)"),
            ("DVORAK-QWERTYCMD", "us(dvorak)"), ("Dvorak-Left", "us(dvorak-l)"), ("Dvorak-Right", "us(dvorak-r)"),
            ("Serbian-Latin", "rs(latin)"), ("Sinhala", "lk(sin_phonetic)"), ("NorthernSami", "no(smi)"),
            ("Hungarian-QWERTY", "hu(qwerty)"), ("Slovak-QWERTY", "sk(qwerty)"),
            ("Bulgarian-Phonetic", "bg(phonetic)"), ("Hebrew-QWERTY", "il(phonetic)"), ("Arabic-AZERTY", "ara(azerty)")
        ] {
            if xkb(id: "com.apple.keylayout." + name, languages: []) != want {
                problems.append("keyboard layout \(name): expected \(want)")
            }
        }
        for language in ["bn", "ur", "sq", "cy", "ga", "mi", "ta", "te", "gu", "km", "lo", "ne", "tk", "ky", "sr-Latn"] {
            if xkb(id: "unknown", languages: [language]) != nil {
                problems.append("keyboard layout: guessed \(language)")
            }
        }
        if request(specs: ["us", "ru", "de", "fr", "es"], active: "es") != "keyboard-layout 3 us ru de es" {
            problems.append("keyboard layout: active fifth layout lost")
        }
        return problems
    }
}
