import AppKit
import Combine
import Foundation

/// Frames of the `fx.clipboard` port, shared with the guest agent
/// (guest/progress-agent/src/clipboard.rs has the full description):
///
///   "FXCB" | type u8 | flags u8 | reserved u16 | seq u32 | length u32 | payload   (little endian)
///
///   1 HELLO  guest → host: protocol version u32 + agent description
///   2 STATE  host → guest: u8 1 = sharing on, 0 = off
///   3 SET    either way: u16 count, per item u16 mime length, mime, u32 length, data
///            (`text/plain;charset=utf-8`, `image/png`)
///   4 ACK    answer to SET `seq`: u32 seq + u8 status (0 applied, 1 already the content, 2 rejected)
enum ClipCodec {
    static let magic: [UInt8] = Array("FXCB".utf8)
    static let header = 16
    static let textMax = 1 << 20
    static let imageMax = 16 << 20
    static let maxPayload = textMax + imageMax + 1024
    static let mimeText = "text/plain;charset=utf-8"
    static let mimePNG = "image/png"

    enum Kind: UInt8 { case hello = 1, state = 2, set = 3, ack = 4 }
    enum Ack: UInt8 { case applied = 0, same = 1, rejected = 2 }

    struct Frame: Equatable {
        let kind: Kind
        let seq: UInt32
        let payload: Data
    }

    static func encode(_ kind: Kind, seq: UInt32 = 0, _ payload: Data) -> Data {
        var d = Data(capacity: header + payload.count)
        d.append(contentsOf: magic)
        d.append(contentsOf: [kind.rawValue, 0, 0, 0])
        d.append(le: seq)
        d.append(le: UInt32(payload.count))
        d.append(payload)
        return d
    }

    static func setPayload(text: Data?, png: Data?) -> Data {
        let items = [(mimeText, text), (mimePNG, png)].compactMap { m, d in d.map { (m, $0) } }
        var p = Data()
        p.append(le: UInt16(items.count))
        for (mime, data) in items {
            p.append(le: UInt16(mime.utf8.count))
            p.append(contentsOf: Array(mime.utf8))
            p.append(le: UInt32(data.count))
            p.append(data)
        }
        return p
    }

    /// SET payload → (text, png); nil if malformed.
    static func parseSet(_ p: Data) -> (text: Data?, png: Data?)? {
        let b = [UInt8](p)
        var at = 0
        func take(_ n: Int) -> ArraySlice<UInt8>? {
            guard n >= 0, at + n <= b.count else { return nil }
            defer { at += n }
            return b[at..<at + n]
        }
        func u16() -> Int? { take(2).map { Int($0.first!) | Int($0.last!) << 8 } }
        func u32() -> Int? { take(4).map { $0.enumerated().reduce(0) { $0 | Int($1.element) << (8 * $1.offset) } } }
        guard let n = u16() else { return nil }
        var text: Data?, png: Data?
        for _ in 0..<n {
            guard let ml = u16(), let mime = take(ml), let dl = u32(), let data = take(dl) else { return nil }
            switch String(decoding: mime, as: UTF8.self) {
            case mimeText: text = Data(data)
            case mimePNG: png = Data(data)
            default: break
            }
        }
        return at == b.count ? (text, png) : nil
    }

    /// FNV-1a over the items (same layout as the guest's), for echo suppression.
    static func hash(text: Data?, png: Data?) -> UInt64 {
        var h: UInt64 = 0xcbf29ce484222325
        func feed<S: Sequence>(_ s: S) where S.Element == UInt8 { for b in s { h = (h ^ UInt64(b)) &* 0x100000001b3 } }
        for (tag, part) in [(UInt8(ascii: "T"), text), (UInt8(ascii: "I"), png)] {
            guard let part else { continue }
            feed([tag])
            var n = UInt64(part.count).littleEndian
            feed(withUnsafeBytes(of: &n) { Array($0) })
            feed(part)
        }
        return h
    }

    static func describe(text: Data?, png: Data?) -> String {
        [text.map { "text \(size($0.count))" }, png.map { "image \(size($0.count))" }].compactMap { $0 }.joined(separator: ", ")
    }

    static func size(_ n: Int) -> String {
        n < 1024 ? "\(n) B" : n < 1 << 20 ? String(format: "%.1f KiB", Double(n) / 1024) : String(format: "%.1f MiB", Double(n) / 1048576)
    }

    /// Incremental decoder; resynchronises on the magic after garbage.
    struct Decoder {
        private var buf: [UInt8] = []

        mutating func feed(_ bytes: UnsafeRawBufferPointer) -> [Frame] {
            buf.append(contentsOf: bytes)
            var out: [Frame] = []
            while true {
                guard let start = Decoder.find(magic, in: buf) else {
                    buf.removeFirst(max(0, buf.count - 3))   // keep a possible partial magic
                    return out
                }
                if start > 0 { buf.removeFirst(start) }
                guard buf.count >= header else { return out }
                let len = Int(buf[12]) | Int(buf[13]) << 8 | Int(buf[14]) << 16 | Int(buf[15]) << 24
                guard let kind = Kind(rawValue: buf[4]), buf[5] == 0, buf[6] == 0, buf[7] == 0, len <= maxPayload else {
                    buf.removeFirst()   // not a frame start
                    continue
                }
                guard buf.count >= header + len else { return out }
                let seq = UInt32(buf[8]) | UInt32(buf[9]) << 8 | UInt32(buf[10]) << 16 | UInt32(buf[11]) << 24
                out.append(Frame(kind: kind, seq: seq, payload: Data(buf[header..<header + len])))
                buf.removeFirst(header + len)
            }
        }

        private static func find(_ needle: [UInt8], in hay: [UInt8]) -> Int? {
            guard hay.count >= needle.count else { return nil }
            return (0...hay.count - needle.count).first { i in hay[i] == needle[0] && Array(hay[i..<i + needle.count]) == needle }
        }
    }
}

private extension Data {
    mutating func append<T: FixedWidthInteger>(le v: T) {
        var x = v.littleEndian
        Swift.withUnsafeBytes(of: &x) { append(contentsOf: $0) }
    }
}

/// The `fx.clipboard` virtio-console port to the guest's fx-clipboard-agent (user service of the
/// gamescope session and of Desktop Mode). Reads on a thread; writes on another (whole frames,
/// blocking: a guest that does not read never stalls the main thread; a newer SET replaces a
/// queued one).
final class ClipboardPort {
    static let name = "fx.clipboard"
    /// Handed to libkrun: guest → host data is written here.
    let guestOutputFd: Int32
    /// Handed to libkrun: host → guest data is read from here.
    let guestInputFd: Int32
    private let readFd: Int32
    private let writeFd: Int32
    private let cond = NSCondition()
    private var queue: [(kind: ClipCodec.Kind, data: Data)] = []
    private var writerStarted = false
    /// The write pipe failed with a non-EINTR error and was logged (writer is restarted, not dead).
    private var writerFailed = false
    /// A full write queue was logged once (frames are dropped under a stall).
    private var droppedQueueLogged = false

    init() throws {
        var out: [Int32] = [0, 0], inp: [Int32] = [0, 0]
        guard pipe(&out) == 0, pipe(&inp) == 0 else { throw OptionError("pipe: \(String(cString: strerror(errno)))") }
        readFd = out[0]; guestOutputFd = out[1]
        guestInputFd = inp[0]; writeFd = inp[1]
        for fd in out + inp { _ = fcntl(fd, F_SETFD, FD_CLOEXEC) }
    }

    func send(_ kind: ClipCodec.Kind, seq: UInt32 = 0, _ payload: Data) {
        let frame = ClipCodec.encode(kind, seq: seq, payload)
        cond.lock()
        if kind == .set { queue.removeAll { $0.kind == .set } }   // only the newest content matters
        if queue.count < 16 {
            queue.append((kind, frame))
        } else if kind == .ack || kind == .state {
            // Acks and state (echo suppression, sharing on/off) are
            // load-bearing, not replaceable like a newer .set: make room by
            // dropping the oldest instead of the ack.
            queue.removeFirst()
            queue.append((kind, frame))
            if !droppedQueueLogged {
                droppedQueueLogged = true
                log("clipboard: write queue full; dropped an older frame to keep a \(kind)")
            }
        } else if !droppedQueueLogged {
            droppedQueueLogged = true
            log("clipboard: write queue full; dropped a \(kind) frame")
        }
        if !writerStarted {
            writerStarted = true
            let t = Thread { [weak self] in self?.writeLoop() }
            t.name = "fx.clipboard write"
            t.start()
        }
        cond.signal()
        cond.unlock()
    }

    private func writeLoop() {
        while true {
            cond.lock()
            while queue.isEmpty { cond.wait() }
            let frame = queue.removeFirst().data
            cond.unlock()
            let err = writeFrame(frame)
            if err == 0 {
                cond.lock()
                if writerFailed {
                    writerFailed = false
                    log("clipboard: the guest write pipe is working again")
                }
                cond.unlock()
                continue
            }
            // A hard error must not kill the writer for the session: clear `writerStarted` under
            // the lock so the next send starts a new one, and end *this* thread (returning from
            // the write closure alone would leave it looping and race the new writer for the queue).
            cond.lock()
            writerStarted = false
            let first = !writerFailed
            writerFailed = true
            cond.unlock()
            if first { log("clipboard: the guest write pipe failed: \(String(cString: strerror(err))); will retry") }
            return
        }
    }

    /// One whole frame to the guest (blocking: a guest that does not read must not stall the main
    /// thread); 0, or the `errno` of a hard write error.
    private func writeFrame(_ data: Data) -> Int32 {
        data.withUnsafeBytes { p in
            var off = 0
            while off < p.count {
                let n = Darwin.write(writeFd, p.baseAddress! + off, p.count - off)
                if n < 0 {
                    if errno == EINTR { continue }
                    return errno
                }
                off += n
            }
            return 0
        }
    }

    /// Reader thread: every frame goes to `handler` on the main queue.
    func start(_ handler: @escaping (ClipCodec.Frame) -> Void) {
        let t = Thread { [readFd] in
            var decoder = ClipCodec.Decoder()
            var buf = [UInt8](repeating: 0, count: 256 * 1024)
            while true {
                let n = Darwin.read(readFd, &buf, buf.count)
                if n < 0 && errno == EINTR { continue }
                if n <= 0 { break }
                let frames = buf.withUnsafeBytes { decoder.feed(UnsafeRawBufferPointer(rebasing: $0[0..<n])) }
                if !frames.isEmpty { DispatchQueue.main.async { frames.forEach(handler) } }
            }
        }
        t.name = "fx.clipboard read"
        t.start()
    }
}

/// Settings > General "Share clipboard with SteamOS": the Mac's general pasteboard ↔ SteamOS's
/// clipboard (text both ways; PNG images, written to the Mac as PNG + TIFF). The pasteboard's
/// changeCount is checked on a 0.5 s timer only while the app is active, the VM runs and the
/// guest agent is connected, and once on every activation — nothing polls in the background.
/// Items marked concealed or transient (org.nspasteboard.ConcealedType / TransientType: password
/// managers) stay on the Mac unless "Include concealed (password manager) items" is on.
/// Echo suppression: the content last sent or taken over (hash) is never sent again, and the
/// changeCount of our own pasteboard write is skipped.
final class ClipboardSync {
    static let pollInterval: TimeInterval = 0.5
    static let concealedTypes: [NSPasteboard.PasteboardType] = [
        .init("org.nspasteboard.ConcealedType"), .init("org.nspasteboard.TransientType"),
    ]

    private let port: ClipboardPort
    private let pasteboard: NSPasteboard
    private var enabled: Bool
    private var includeConcealed: Bool
    private var appActive = true
    private var guestConnected = false
    /// The whole VM is suspended or asleep (SuspendController): nothing is checked or sent.
    var vmPaused = false {
        didSet {
            guard vmPaused != oldValue else { return }
            updateTimer()
            if !vmPaused { check() }
        }
    }
    private var lastChangeCount: Int
    /// changeCount whose concealed item was already logged as kept on the Mac.
    private var loggedConcealed = -1
    private var lastHash: UInt64?
    private var seq: UInt32 = 0
    private var timer: DispatchSourceTimer?
    private var observers: [NSObjectProtocol] = []
    private var subscriptions: [AnyCancellable] = []

    init(port: ClipboardPort, settings: LauncherSettings, pasteboard: NSPasteboard = .general) {
        self.port = port
        self.pasteboard = pasteboard
        enabled = settings.shareClipboard
        includeConcealed = settings.shareConcealedClipboard
        lastChangeCount = pasteboard.changeCount
        let nc = NotificationCenter.default
        observers = [
            nc.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
                self?.setAppActive(false)
            },
            nc.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
                self?.setAppActive(true)
            },
        ]
        // @Published emits the new value before it is stored: use the emitted value.
        subscriptions = [
            settings.$shareClipboard.dropFirst().removeDuplicates().sink { [weak self] on in
                DispatchQueue.main.async { self?.setEnabled(on) }
            },
            settings.$shareConcealedClipboard.dropFirst().removeDuplicates().sink { [weak self] on in
                DispatchQueue.main.async { self?.includeConcealed = on }
            },
        ]
        port.start { [weak self] in self?.frame($0) }
    }

    func setAppActive(_ active: Bool) {
        guard active != appActive else { return }
        appActive = active
        if active { check() }
        updateTimer()
    }

    private func setEnabled(_ on: Bool) {
        guard on != enabled else { return }
        enabled = on
        log("clipboard: sharing \(on ? "on" : "off")")
        if guestConnected {
            sendState()
            if on { check(force: true) }
        }
        updateTimer()
    }

    private func sendState() {
        port.send(.state, Data([enabled ? 1 : 0]))
    }

    private func updateTimer() {
        let run = enabled && guestConnected && appActive && !vmPaused
        if run, timer == nil {
            let t = DispatchSource.makeTimerSource(queue: .main)
            t.schedule(deadline: .now() + ClipboardSync.pollInterval, repeating: ClipboardSync.pollInterval, leeway: .milliseconds(200))
            t.setEventHandler { [weak self] in self?.check() }
            t.resume()
            timer = t
        } else if !run, let t = timer {
            t.cancel()
            timer = nil
        }
    }

    /// Items with these types stay on the Mac unless concealed items are included.
    static func isConcealed(_ types: [NSPasteboard.PasteboardType]) -> Bool {
        types.contains { concealedTypes.contains($0) }
    }

    /// Send the pasteboard if it changed (`force`: even if its changeCount did not).
    private func check(force: Bool = false) {
        let cc = pasteboard.changeCount
        guard force || cc != lastChangeCount else { return }
        lastChangeCount = cc
        guard enabled, guestConnected, !vmPaused else { return }
        let types = pasteboard.types ?? []
        if ClipboardSync.isConcealed(types) && !includeConcealed {
            if loggedConcealed != cc {
                loggedConcealed = cc
                log("clipboard: Mac item marked concealed/transient (password manager): kept on the Mac")
            }
            return
        }
        let text = pasteboard.string(forType: .string).map { Data($0.utf8) }
        var png: Data?
        // A copied Finder file is its name (text), not its icon.
        var tiff: Data?
        if !types.contains(.fileURL) {
            if let p = pasteboard.data(forType: .png) {
                png = p
            } else if types.contains(.tiff) || NSImage.canInit(with: pasteboard),
                      let image = NSImage(pasteboard: pasteboard), let t = image.tiffRepresentation {
                tiff = t
            }
        }
        // The pasteboard is read on the main thread, but a TIFF-only item is
        // re-encoded to PNG on a background queue: encoding a 4K screenshot
        // takes tens of ms and would drop a guest frame on every copy.
        guard let t = tiff else {
            finish(text: text, png: png, cc: cc)
            return
        }
        DispatchQueue.global(qos: .utility).async { [weak self] in
            let encoded = NSBitmapImageRep(data: t)?.representation(using: .png, properties: [:])
            DispatchQueue.main.async {
                self?.finish(text: text, png: encoded, cc: cc)
            }
        }
    }

    /// Limit checks, echo suppression and the send, after any off-main image
    /// encode; `cc` drops the item if the pasteboard changed meanwhile.
    private func finish(text: Data?, png: Data?, cc: Int) {
        guard cc == lastChangeCount, enabled, guestConnected, !vmPaused else { return }
        var text = text
        var png = png
        if let t = text, t.count > ClipCodec.textMax {
            log("clipboard: Mac text of \(ClipCodec.size(t.count)) exceeds the \(ClipCodec.size(ClipCodec.textMax)) limit: not shared")
            text = nil
        }
        if let p = png, p.count > ClipCodec.imageMax {
            log("clipboard: Mac image of \(ClipCodec.size(p.count)) (PNG) exceeds the \(ClipCodec.size(ClipCodec.imageMax)) limit: not shared")
            png = nil
        }
        if text?.isEmpty == true { text = nil }
        guard text != nil || png != nil else { return }
        let h = ClipCodec.hash(text: text, png: png)
        guard h != lastHash else { return }
        lastHash = h
        seq &+= 1
        log("clipboard: Mac → SteamOS #\(seq): \(ClipCodec.describe(text: text, png: png))")
        port.send(.set, seq: seq, ClipCodec.setPayload(text: text, png: png))
    }

    private func frame(_ f: ClipCodec.Frame) {
        switch f.kind {
        case .hello:
            let version = f.payload.count >= 4 ? Int(f.payload[0]) | Int(f.payload[1]) << 8 : 0
            let agent = String(decoding: f.payload.dropFirst(4), as: UTF8.self)
            log("clipboard: guest agent connected (\(agent), protocol \(version)); sharing \(enabled ? "on" : "off")")
            guestConnected = true
            // A restarted agent has no content: the Mac's current one goes to it.
            lastHash = nil
            sendState()
            if enabled { check(force: true) }
            updateTimer()
        case .set:
            guestSet(f)
        case .ack:
            guard f.payload.count >= 5 else { return }
            let s = f.payload.startIndex
            let n = UInt32(f.payload[s]) | UInt32(f.payload[s + 1]) << 8 | UInt32(f.payload[s + 2]) << 16 | UInt32(f.payload[s + 3]) << 24
            let what: String
            switch ClipCodec.Ack(rawValue: f.payload[s + 4]) {
            case .applied: what = "on SteamOS's clipboard"
            case .same: what = "already SteamOS's clipboard"
            default: what = "rejected by SteamOS"
            }
            log("clipboard: Mac → SteamOS #\(n): \(what)")
        case .state:
            break
        }
    }

    private func ack(_ seq: UInt32, _ status: ClipCodec.Ack) {
        var p = Data()
        p.append(le: seq)
        p.append(status.rawValue)
        port.send(.ack, p)
    }

    private func guestSet(_ f: ClipCodec.Frame) {
        guard enabled else {
            log("clipboard: SteamOS → Mac #\(f.seq) while sharing is off: ignored")
            return ack(f.seq, .rejected)
        }
        guard let (text, png) = ClipCodec.parseSet(f.payload), text != nil || png != nil,
              (text?.count ?? 0) <= ClipCodec.textMax, (png?.count ?? 0) <= ClipCodec.imageMax else {
            log("clipboard: SteamOS → Mac #\(f.seq): malformed or over the limits, ignored")
            return ack(f.seq, .rejected)
        }
        let h = ClipCodec.hash(text: text, png: png)
        guard h != lastHash else { return ack(f.seq, .same) }
        let item = NSPasteboardItem()
        if let text { item.setString(String(decoding: text, as: UTF8.self), forType: .string) }
        if let png {
            item.setData(png, forType: .png)
            if let tiff = NSBitmapImageRep(data: png)?.tiffRepresentation { item.setData(tiff, forType: .tiff) }
        }
        pasteboard.clearContents()
        guard pasteboard.writeObjects([item]) else {
            log("clipboard: SteamOS → Mac #\(f.seq): writing the pasteboard failed")
            return ack(f.seq, .rejected)
        }
        lastChangeCount = pasteboard.changeCount
        lastHash = h
        log("clipboard: SteamOS → Mac #\(f.seq): \(ClipCodec.describe(text: text, png: png))")
        ack(f.seq, .applied)
    }

    /// --selftest-settings: frame codec round trip / resync and the concealed-type filter.
    static func selfCheck() -> [String] {
        var failures: [String] = []
        let text = Data("привет 🎮 clipboard".utf8), png = Data([0x89, 0x50, 0x4e, 0x47, 0x46, 0x58, 0x43, 0x42])
        var stream = Data("garbage FXC".utf8)
        stream.append(ClipCodec.encode(.set, seq: 7, ClipCodec.setPayload(text: text, png: png)))
        stream.append(ClipCodec.encode(.state, Data([1])))
        var decoder = ClipCodec.Decoder()
        var frames: [ClipCodec.Frame] = []
        for b in stream { frames += withUnsafeBytes(of: b) { decoder.feed($0) } }
        if frames.count != 2 || frames.first?.kind != .set || frames.first?.seq != 7 || frames.last != .init(kind: .state, seq: 0, payload: Data([1])) {
            failures.append("clipboard: frames not decoded (\(frames.map { "\($0.kind)#\($0.seq)" }))")
        } else if let (t, p) = ClipCodec.parseSet(frames[0].payload), t == text, p == png {
            if ClipCodec.hash(text: t, png: p) == ClipCodec.hash(text: p, png: t) { failures.append("clipboard: hash ignores the item kind") }
        } else {
            failures.append("clipboard: SET payload did not round-trip")
        }
        if ClipCodec.parseSet(Data([1, 0, 3, 0])) != nil { failures.append("clipboard: truncated SET accepted") }
        if !isConcealed([.string, .init("org.nspasteboard.ConcealedType")]) || !isConcealed([.init("org.nspasteboard.TransientType")])
            || isConcealed([.string, .png]) {
            failures.append("clipboard: concealed/transient classification")
        }
        return failures
    }
}
