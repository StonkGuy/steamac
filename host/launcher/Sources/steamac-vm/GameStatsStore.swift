import AppKit
import Foundation
import Combine
import SwiftUI

/// Every game session recorded on this Mac (GameStats.swift), the player's ratings and per-game
/// sharing switches, in `<Application Support>/es.fxgam.steamac/game-stats.json`
/// (STEAMAC_GAME_STATS_FILE overrides), and their upload to the statistics service while Settings
/// `shareGameStats` is on. Main thread only.
///
/// Upload: one request at a time, sessions first, then ratings. A session recorded while sharing
/// was off stays pending and goes out once it is on (the consent panel says so); a game whose
/// sharing switch is off sends nothing, and its rating is withdrawn from the service (`none`).
/// 2xx = done; 400/413 = the service will never take it (dropped, logged); anything else (429,
/// 5xx, network) = retried 10 minutes later or at the next session end / boot. The service URL is
/// Info.plist `SteamacGameStatsURL`; development builds (work/out/steamac-vm) upload only with
/// STEAMAC_GAME_STATS_URL set (also the test hook).
final class GameStatsStore: ObservableObject {
    static let shared = GameStatsStore()
    static let urlEnv = "STEAMAC_GAME_STATS_URL", fileEnv = "STEAMAC_GAME_STATS_FILE"
    /// The public statistics page, opened with this Mac's chip, GPU cores, RAM and performance
    /// cores: its "My Mac" filter then shows only reports from Macs that are not better.
    static let pageURL: URL = {
        let mac = GameStatsMac.current
        var c = URLComponents(string: "https://fxgl.github.io/steamac/games.html")!
        c.queryItems = [URLQueryItem(name: "chip", value: mac.chip), URLQueryItem(name: "ram", value: String(mac.ramGB))]
            + (mac.gpuCores.map { [URLQueryItem(name: "gpu", value: String($0))] } ?? [])
            + (mac.pcores.map { [URLQueryItem(name: "pcores", value: String($0))] } ?? [])
        return c.url!
    }()
    /// Recorded sessions kept per game (newest first).
    static let sessionsPerGame = 20
    static let retryInterval: TimeInterval = 10 * 60

    static var launcherVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    /// The service's base URL, nil when this build does not upload.
    static var serviceURL: URL? {
        if let s = ProcessInfo.processInfo.environment[urlEnv], !s.isEmpty { return URL(string: s) }
        guard CrashReporting.buildKind != .development else { return nil }
        return (Bundle.main.object(forInfoDictionaryKey: "SteamacGameStatsURL") as? String).flatMap(URL.init(string:))
    }

    enum SessionState: String, Codable {
        /// Not uploaded yet.
        case pending
        case sent
        /// The service refused it (400/413): never retried.
        case rejected
    }

    struct StoredSession: Codable, Identifiable, Equatable {
        var report: GameSessionReport
        var ended: Date
        var state: SessionState
        var id: String { report.sid }
    }

    struct Game: Codable, Identifiable, Equatable {
        let appid: Int
        var name: String?
        /// Per-game switch: off = nothing about this game leaves the Mac.
        var sharing = true
        var rating: GameRating?
        /// The rating the service has from this Mac (nil = none).
        var sentRating: GameRating?
        var totalSessions = 0
        var totalPlayS = 0
        var lastPlayed: Date?
        var sessions: [StoredSession] = []
        var id: Int { appid }

        var title: String { name ?? String(localized: "App \(String(appid))", comment: "Game without a known name; Steam app ID") }
    }

    private struct File: Codable {
        var install: String
        /// The one-time sharing question was answered (GameStatsConsentPanel).
        var asked: Bool
        var games: [Game]
    }

    /// Newest-played first.
    @Published private(set) var games: [Game] = []
    /// Upload state for Settings > Games ("" = nothing to say).
    @Published private(set) var status = ""
    /// Random per-installation id: distinct-install counts and rate limits on the service only.
    let install: String
    private(set) var asked: Bool
    private let path: String
    private var busy = false
    /// The last upload attempt failed and waits for its retry.
    private var failing = false
    private var retry: Timer?
    private var panel: GameStatsConsentPanel?
    private var settingObserver: Any?

    private init() {
        let path = ProcessInfo.processInfo.environment[GameStatsStore.fileEnv].flatMap { $0.isEmpty ? nil : $0 }
            ?? AppBundle.appSupportDir + "/game-stats.json"
        self.path = path
        let file = FileManager.default.contents(atPath: path).flatMap { data -> File? in
            let d = JSONDecoder()
            d.dateDecodingStrategy = .iso8601
            do { return try d.decode(File.self, from: data) } catch {
                log("game stats: \(path) unreadable (\(error)); starting over")
                return nil
            }
        }
        install = file?.install ?? UUID().uuidString
        asked = file?.asked ?? false
        games = GameStatsStore.sorted(file?.games ?? [])
        settingObserver = LauncherSettings.shared.$shareGameStats.dropFirst().removeDuplicates().sink { [weak self] on in
            DispatchQueue.main.async {
                log("game stats: sharing \(on ? "on" : "off")")
                guard let self else { return }
                // Turned on in Settings: the one-time question is answered as well.
                if on && !self.asked {
                    self.asked = true
                    self.save()
                }
                self.flush()
                self.updateStatus()
            }
        }
        updateStatus()
    }

    private static func sorted(_ games: [Game]) -> [Game] {
        games.sorted { ($0.lastPlayed ?? .distantPast) > ($1.lastPlayed ?? .distantPast) }
    }

    private func save() {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .iso8601
        e.outputFormatting = [.prettyPrinted, .sortedKeys]
        do {
            try FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
            try e.encode(File(install: install, asked: asked, games: games)).write(to: URL(fileURLWithPath: path), options: .atomic)
        } catch {
            log("game stats: cannot save \(path): \(error)")
        }
    }

    private func update(_ appid: Int, _ body: (inout Game) -> Void) {
        var all = games
        if let i = all.firstIndex(where: { $0.appid == appid }) {
            body(&all[i])
        } else {
            var g = Game(appid: appid)
            body(&g)
            all.append(g)
        }
        games = GameStatsStore.sorted(all)
        save()
        updateStatus()
    }

    // MARK: inputs

    /// A finished session (GameSessionTracker).
    func record(_ report: GameSessionReport) {
        update(report.appid) { g in
            if let name = report.game { g.name = name }
            g.totalSessions += 1
            g.totalPlayS += report.playS
            g.lastPlayed = Date()
            g.sessions.insert(StoredSession(report: report, ended: Date(), state: .pending), at: 0)
            if g.sessions.count > GameStatsStore.sessionsPerGame { g.sessions.removeLast(g.sessions.count - GameStatsStore.sessionsPerGame) }
        }
        flush()
    }

    func setRating(_ rating: GameRating?, for appid: Int) {
        update(appid) { $0.rating = rating }
        log("game stats: \(appid) rated \(rating?.rawValue ?? "none")")
        flush()
    }

    func setSharing(_ on: Bool, for appid: Int) {
        update(appid) { $0.sharing = on }
        log("game stats: sharing for \(appid) \(on ? "on" : "off")")
        flush()
    }

    // MARK: upload

    private enum Job {
        case session(appid: Int, sid: String, body: Data)
        case rating(appid: Int, rating: GameRating?, body: Data)
    }

    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = [.sortedKeys]
        return e
    }()

    /// What the service should have for `g`: its rating while the game is shared.
    private func wantedRating(_ g: Game) -> GameRating? { g.sharing ? g.rating : nil }

    /// Sessions of the experimental Mullion path (this fork) stay on this Mac: the service's numbers are for the
    /// paths its users can choose.
    static func uploadable(gfx: String?) -> Bool { gfx != "mullion" }

    private func nextJob() -> Job? {
        for g in games where g.sharing {
            if let s = g.sessions.last(where: { $0.state == .pending && Self.uploadable(gfx: $0.report.gfx) }),
               let body = try? GameStatsStore.encoder.encode(s.report) {
                return .session(appid: g.appid, sid: s.report.sid, body: body)
            }
        }
        for g in games where wantedRating(g) != g.sentRating {
            let wanted = wantedRating(g)
            let last = g.sessions.first(where: { Self.uploadable(gfx: $0.report.gfx) })?.report
            let report = GameRatingReport(
                install: install, launcher: GameStatsStore.launcherVersion, appid: g.appid, game: g.name,
                rating: wanted?.rawValue ?? "none", mac: .current,
                vm: GameStatsVM(driver: last?.vm.driver ?? LauncherSettings.shared.vulkanDriver.rawValue,
                                wmtr: last?.vm.wmtr ?? LauncherSettings.shared.wmtr),
                gfx: last?.gfx, env: last?.env)
            if let body = try? GameStatsStore.encoder.encode(report) { return .rating(appid: g.appid, rating: wanted, body: body) }
        }
        return nil
    }

    var pendingCount: Int {
        games.filter(\.sharing).reduce(0) { $0 + $1.sessions.filter { $0.state == .pending && Self.uploadable(gfx: $0.report.gfx) }.count }
    }

    /// Upload whatever is due, one request at a time (no-op while sharing is off).
    func flush() {
        guard !busy, LauncherSettings.shared.shareGameStats, let base = GameStatsStore.serviceURL,
              let job = nextJob() else { return }
        busy = true
        retry?.invalidate()
        retry = nil
        let (url, body): (URL, Data)
        switch job {
        case .session(_, _, let b): (url, body) = (base.appendingPathComponent("v1/sessions"), b)
        case .rating(_, _, let b): (url, body) = (base.appendingPathComponent("v1/ratings"), b)
        }
        var req = URLRequest(url: url, timeoutInterval: 20)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue("FXSteamLauncher/\(GameStatsStore.launcherVersion)", forHTTPHeaderField: "User-Agent")
        req.httpBody = body
        GameStatsStore.session.dataTask(with: req) { data, response, error in
            let code = (response as? HTTPURLResponse)?.statusCode
            let detail = error?.localizedDescription
                ?? data.flatMap { String(data: $0.prefix(200), encoding: .utf8) } ?? ""
            DispatchQueue.main.async { self.finished(job, code: code, detail: detail) }
        }.resume()
    }

    /// No cookies, no cache, no credentials.
    private static let session = URLSession(configuration: .ephemeral)

    private func finished(_ job: Job, code: Int?, detail: String) {
        busy = false
        let done = code.map { (200..<300).contains($0) } ?? false
        let refused = code == 400 || code == 413
        failing = !(done || refused)
        switch job {
        case .session(let appid, let sid, _):
            if done || refused {
                update(appid) { g in
                    if let i = g.sessions.firstIndex(where: { $0.report.sid == sid }) { g.sessions[i].state = done ? .sent : .rejected }
                }
                log("game stats: session \(sid.prefix(8)) of \(appid) \(done ? "sent" : "refused (HTTP \(code!)): \(detail)")")
            }
        case .rating(let appid, let rating, _):
            if done || refused {
                update(appid) { $0.sentRating = rating }
                log("game stats: rating \(rating?.rawValue ?? "none") of \(appid) \(done ? "sent" : "refused (HTTP \(code!)): \(detail)")")
            }
        }
        if done || refused {
            flush()
            return
        }
        log("game stats: upload failed (\(code.map { "HTTP \($0)" } ?? "no answer"): \(detail)); retrying in \(Int(GameStatsStore.retryInterval / 60)) min")
        updateStatus()
        retry = Timer.scheduledTimer(withTimeInterval: GameStatsStore.retryInterval, repeats: false) { [weak self] _ in self?.flush() }
    }

    private func updateStatus() {
        let pending = pendingCount
        if failing && LauncherSettings.shared.shareGameStats {
            status = String(localized: "The statistics service is not reachable. Sessions waiting to be sent: \(pending).")
            return
        }
        if !LauncherSettings.shared.shareGameStats {
            status = pending > 0 ? String(localized: "Recorded sessions kept on this Mac: \(pending).") : ""
        } else if GameStatsStore.serviceURL == nil {
            status = String(localized: "This development build does not send statistics.")
        } else {
            status = pending > 0 ? String(localized: "Sessions waiting to be sent: \(pending).") : ""
        }
    }

    // MARK: one-time question

    /// After the first game the player saw: ask once whether to share (never again once answered).
    func offerSharingIfNeeded(beside vm: NSWindow?) {
        guard !asked, !LauncherSettings.shared.shareGameStats else { return }
        showConsent(beside: vm)
    }

    func showConsent(beside vm: NSWindow?) {
        let panel = self.panel ?? GameStatsConsentPanel { [weak self] share in self?.answer(share) }
        self.panel = panel
        panel.show(beside: vm)
        log("game stats: asking whether to share")
    }

    private func answer(_ share: Bool) {
        asked = true
        save()
        panel?.window.orderOut(nil)
        log("game stats: the user chose \(share ? "to share" : "not to share")")
        if LauncherSettings.shared.shareGameStats != share { LauncherSettings.shared.shareGameStats = share }
    }

    /// Control FIFO `stats …`: state | flush | ask | press share|decline | dump PATH | reset-question |
    /// rate APPID RATING|none | share APPID on|off (as the Settings > Games row's controls).
    func control(_ args: [String], tracker: GameSessionTracker?) {
        switch args.first {
        case "state":
            // Built in steps to stay within the type checker's time limit (GameStats.swift end(_:_:)).
            let sharing: String = LauncherSettings.shared.shareGameStats ? "on" : "off"
            let service: String = GameStatsStore.serviceURL?.absoluteString ?? "none"
            let live: String = tracker?.describe() ?? "-"
            var state = "control: stats: sharing \(sharing), asked \(asked), service \(service), file \(path), "
            state += "pending \(pendingCount), busy \(busy), status \"\(status)\"; live: \(live)"
            log(state)
            for g in games {
                let name: String = g.name ?? ""
                let rating: String = g.rating?.rawValue ?? "-"
                let sent: String = g.sentRating?.rawValue ?? "-"
                let sessions: String = g.sessions.map { "\($0.report.sid.prefix(8)):\($0.state.rawValue)" }.joined(separator: ",")
                var line = "control: stats: \(g.appid) \"\(name)\" sharing \(g.sharing) rating \(rating) "
                line += "sent rating \(sent) sessions \(g.totalSessions) \(sessions)"
                log(line)
            }
        case "flush": flush()
        case "ask": showConsent(beside: NSApp.windows.first { $0.isVisible && $0 !== panel?.window })
        case "press":
            guard panel?.window.isVisible == true else { log("control: stats: the question is not shown"); return }
            switch args.dropFirst().first {
            case "share": answer(true)
            case "decline": answer(false)
            default: log("control: stats: press share|decline")
            }
        case "dump":
            guard let path = args.dropFirst().first, let w = panel?.window,
                  let png = SettingsWindowController.snapshot(w)?.representation(using: .png, properties: [:]),
                  (try? png.write(to: URL(fileURLWithPath: path))) != nil else { log("control: stats: dump failed"); return }
            log("control: stats: question dumped to \(path)")
        case "reset-question":
            asked = false
            save()
        case "rate":
            guard args.count == 3, let id = Int(args[1]), args[2] == "none" || GameRating(rawValue: args[2]) != nil else {
                log("control: stats: rate APPID well|issues|unplayable|no_start|none"); return
            }
            setRating(GameRating(rawValue: args[2]), for: id)
        case "share":
            guard args.count == 3, let id = Int(args[1]), ["on", "off"].contains(args[2]) else {
                log("control: stats: share APPID on|off"); return
            }
            setSharing(args[2] == "on", for: id)
        default:
            log("control: stats: unknown command")
        }
    }
}

/// The one-time "Share game statistics?" question: a panel beside the VM window that does not
/// take the keyboard or a captured mouse (as UpdatePanel).
final class GameStatsConsentPanel {
    let window: NSPanel
    static let width: CGFloat = 480

    init(answer: @escaping (Bool) -> Void) {
        window = NSPanel(contentRect: NSRect(x: 0, y: 0, width: GameStatsConsentPanel.width, height: 200),
                         styleMask: [.titled], backing: .buffered, defer: false)
        window.title = String(localized: "Game Statistics")
        window.isReleasedWhenClosed = false
        window.hidesOnDeactivate = false
        window.becomesKeyOnlyIfNeeded = true
        window.contentView = NSHostingView(rootView: GameStatsConsentView(answer: answer))
    }

    func show(beside vm: NSWindow?) {
        guard let host = window.contentView else { return }
        host.layoutSubtreeIfNeeded()
        var size = host.fittingSize
        size.width = GameStatsConsentPanel.width
        let frame = window.frameRect(forContentRect: NSRect(origin: .zero, size: size))
        window.setFrame(UpdatePanel.placement(frame.size, beside: vm), display: false)
        window.orderFront(nil)
    }
}

/// What sharing sends and never sends (consent panel, Settings > Games "What Is Sent").
enum GameStatsText {
    static let summary = String(localized: "How your games run in SteamOS goes to the public game statistics, so every Mac player can see which games start and how fast they run on which Mac.")
    static let whatIsSent = [
        String(localized: "Each game session: the game's Steam app ID and name, how long it ran and was played, whether its window appeared, frame rate, memory use, Proton version, resolution, graphics path and sanitized graphics launch settings."),
        String(localized: "This Mac: model, chip, CPU and GPU cores, memory, macOS version; the VM's size, Vulkan driver and SteamOS build; the launcher version."),
        String(localized: "Your rating of a game, if you set one in Settings > Games."),
        String(localized: "A random install ID (not linked to you or to crash reports) to count Macs."),
        String(localized: "Never: your name, user or computer name, IP address, Steam account, files or other text you type."),
    ]
}

private struct GameStatsConsentView: View {
    let answer: (Bool) -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            Image(systemName: "chart.bar.xaxis").font(.system(size: 34)).foregroundStyle(.tint).frame(width: 48)
            VStack(alignment: .leading, spacing: 10) {
                Text("Share game statistics?").font(.headline)
                Text(verbatim: GameStatsText.summary).fixedSize(horizontal: false, vertical: true)
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(GameStatsText.whatIsSent, id: \.self) { line in
                        Text(verbatim: "• " + line).font(.caption).foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                Text("Sessions already recorded on this Mac are included. You can rate games, turn sharing off for single games or altogether in Settings > Games.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                HStack {
                    Link("Open Statistics Page", destination: GameStatsStore.pageURL).font(.caption)
                    Spacer()
                    Button("Don't Share") { answer(false) }.keyboardShortcut(.cancelAction)
                    Button("Share") { answer(true) }.keyboardShortcut(.defaultAction)
                }
            }
        }
        .padding(20)
        .frame(width: GameStatsConsentPanel.width, alignment: .topLeading)
    }
}
