import AppKit
import Darwin
import Foundation
import SwiftUI

/// Settings > VR: what steamac-vrhost is doing (headset, stream, pairing) and how to set up a headset.
/// The data comes from the host's control socket (docs/vr-interfaces.md section 5): AF_UNIX SOCK_STREAM at
/// `VRControl.socketPath`, one request line, one JSON line back. Without a running host, the paired list and
/// unpairing go through the bundled executable (`--list-headsets --json`, `--unpair NAME`), which edits the same
/// store (~/Library/Application Support/steamac/vrhost/known_keys).
enum VRControl {
    /// The host's control socket (VRHost.start passes it as --control-socket); one VR host per user at a time.
    static var socketPath: String {
        NSHomeDirectory() + "/Library/Application Support/steamac/vrhost/control.sock"
    }

    /// One request → the reply line, or nil when no host answers within `timeout`.
    static func request(_ line: String, path: String = socketPath, timeout: TimeInterval = 1) -> Data? {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var tv = timeval(tv_sec: Int(timeout), tv_usec: Int32((timeout - floor(timeout)) * 1e6))
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let bytes = Array(path.utf8)
        guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else { return nil }
        withUnsafeMutableBytes(of: &addr.sun_path) { buf in
            buf.copyBytes(from: bytes)
            buf[bytes.count] = 0
        }
        let ok = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) == 0
            }
        }
        guard ok else { return nil }
        let req = Array((line + "\n").utf8)
        guard send(fd, req, req.count, 0) == req.count else { return nil }
        var reply = Data()
        var buf = [UInt8](repeating: 0, count: 4096)
        while reply.count < 1 << 16 {
            let n = recv(fd, &buf, buf.count, 0)
            if n <= 0 { break }
            reply.append(buf, count: n)
            if buf[n - 1] == 0x0a { break }
        }
        return reply.isEmpty ? nil : reply
    }
}

/// The control socket's "status" reply (protocol version 1).
struct VRStatus: Decodable, Equatable {
    struct Headset: Decodable, Equatable {
        var name: String
        var eye_width: Int
        var eye_height: Int
        var render_width: Int?
        var render_height: Int?
        var refresh_hz: Double
        var refresh_rates: [Double]?
        struct Battery: Decodable, Equatable { var charge: Double; var charging: Bool }
        var codec: String?          // hevc | h264 (hosts since vr-polish)
        var battery: Battery?
    }
    /// The last unpaired headset that connected; `refused` = pairing was off and not opened on request.
    struct PairingRequest: Decodable, Equatable { var name: String; var at: Int64; var refused: Bool }
    struct AppRequest: Decodable, Equatable { var id: String; var seq: UInt64 }
    struct Stream: Decodable, Equatable {
        var frames: Int
        var bitrate_mbps: Double
        var target_mbps: Double
        var encode_ms: Double
        var decode_ms: Double
        var pipeline_ms: Double
        // since vr-polish (game path only for the first six)
        var game_fps: Double?
        var received: Int?
        var no_headset: Int?
        var compose_gpu_ms: Double?
        var submit_copied_ms: Double?
        var upscaler: String?
        var idr_requests: Int?
    }
    var v: Int
    var state: String          // waiting | pairing | connected
    var port: Int
    var bonjour: String
    var encryption: String     // enabled | pairing | disabled
    var pin: String
    var headset: Headset?
    var stream: Stream
    var pairing_request: PairingRequest?
    var app_request: AppRequest?
}

struct VRPairedHeadset: Decodable, Equatable, Identifiable {
    var name: String
    var last_connection: Int64
    var id: String { name }
    /// Entries of the same name (the store keeps one per key: a client reinstalled or a test client that
    /// connected many times); unpairing by name removes them all.
    var count = 1

    private enum CodingKeys: String, CodingKey { case name, last_connection }

    init(name: String, last_connection: Int64, count: Int = 1) {
        self.name = name
        self.last_connection = last_connection
        self.count = count
    }

    /// One row per name, newest connection first.
    static func grouped(_ list: [VRPairedHeadset]) -> [VRPairedHeadset] {
        var byName: [String: VRPairedHeadset] = [:]
        var order: [String] = []
        for h in list {
            if var g = byName[h.name] {
                g.count += 1
                g.last_connection = max(g.last_connection, h.last_connection)
                byName[h.name] = g
            } else {
                byName[h.name] = VRPairedHeadset(name: h.name, last_connection: h.last_connection)
                order.append(h.name)
            }
        }
        return order.compactMap { byName[$0] }.sorted { $0.last_connection > $1.last_connection }
    }
}

private struct VRHeadsetList: Decodable { var headsets: [VRPairedHeadset] }
private struct VRError: Decodable { var error: String }

/// Which setup hint applies to a headset (WiVRn's headset_info system_name).
enum VRHeadsetFamily: Equatable {
    case questPico, steamFrame, other

    init(name: String) {
        let n = name.lowercased()
        if ["quest", "meta", "oculus", "pico"].contains(where: n.contains) { self = .questPico }
        else if n.contains("frame") || n.contains("valve") { self = .steamFrame }
        else { self = .other }
    }
}

/// Settings > VR state: polled once a second while the tab is visible. All text derivation is in pure
/// functions (`lines(...)`) so --selftest-vr checks it without a window.
@MainActor
final class VRPanelModel: ObservableObject {
    @Published private(set) var status: VRStatus?
    @Published private(set) var headsets: [VRPairedHeadset] = []
    @Published private(set) var lastError: String?

    let socketPath: String
    /// `nil` = VRHost.path (the bundled executable); the self-test passes its own.
    let executable: () -> String?
    private var timer: Timer?
    private var busy = false

    init(socketPath: String = VRControl.socketPath, executable: @escaping () -> String? = { VRHost.path }) {
        self.socketPath = socketPath
        self.executable = executable
    }

    var hostRunning: Bool { status != nil }

    func start() {
        refresh()
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }

    func refresh() {
        guard !busy else { return }
        busy = true
        let path = socketPath, exe = executable()
        Task.detached {
            let status = VRControl.request("status", path: path).flatMap { try? JSONDecoder().decode(VRStatus.self, from: $0) }
            let list = status != nil
                ? VRControl.request("headsets", path: path).flatMap { try? JSONDecoder().decode(VRHeadsetList.self, from: $0) }?.headsets
                : Self.cliHeadsets(exe)
            await MainActor.run {
                self.status = status
                if let list { self.headsets = VRPairedHeadset.grouped(list) }
                self.busy = false
            }
        }
    }

    /// Pairing on/off through the running host (WiVRn's "enable pairing": a fresh PIN each time).
    func setPairing(_ on: Bool) { send(on ? "pair on" : "pair off") }

    /// Total bitrate of the running host (Settings > VR "Bitrate" applies at once; the next start reads the setting).
    func setBitrate(_ mbps: Int) { if hostRunning { send("bitrate \(mbps)") } }

    /// Unpair one headset ("" = all): through the host when it runs, else the executable.
    func unpair(_ name: String) {
        if hostRunning {
            send(name.isEmpty ? "unpair-all" : "unpair " + name)
            return
        }
        let exe = executable()
        Task.detached {
            if let exe { _ = Self.run(exe, name.isEmpty ? ["--unpair-all"] : ["--unpair", name]) }
            await MainActor.run { self.refresh() }
        }
    }

    private func send(_ line: String) {
        let path = socketPath
        Task.detached {
            let reply = VRControl.request(line, path: path)
            let error = reply.flatMap { try? JSONDecoder().decode(VRError.self, from: $0) }?.error
            await MainActor.run {
                self.lastError = reply == nil ? String(localized: "The VR host did not answer.") : error.map(Self.hostError)
                self.refresh()
            }
        }
    }

    /// The host's control-socket errors (host/vrhost src/control/control_server.cpp) in the user's language;
    /// an error this launcher does not know is shown as the host sent it.
    nonisolated static func hostError(_ error: String) -> String {
        switch error {
        case "encryption is disabled: no pairing": String(localized: "Pairing needs encryption, which is off.")
        case "bitrate: Mbit/s, 1 to 400": String(localized: "The bitrate must be 1 to 400 Mbit/s.")
        case "refresh: rate in Hz": String(localized: "The refresh rate must be given in Hz.")
        case "refresh: no headset connected, or it does not offer this rate":
            String(localized: "No headset is connected, or it does not offer this refresh rate.")
        case "unpair: no name": String(localized: "No headset name given to unpair.")
        case "unknown request": String(localized: "The VR host did not understand the request (a different version?).")
        default: error
        }
    }

    nonisolated static func cliHeadsets(_ exe: String?) -> [VRPairedHeadset]? {
        guard let exe, let out = run(exe, ["--list-headsets", "--json"]) else { return nil }
        return out.split(separator: "\n").compactMap { try? JSONDecoder().decode(VRPairedHeadset.self, from: Data($0.utf8)) }
    }

    nonisolated static func run(_ exe: String, _ args: [String]) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: exe)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return p.terminationStatus == 0 ? String(decoding: data, as: UTF8.self) : nil
    }

    // MARK: text (pure)

    struct Lines: Equatable {
        var host: String
        var headset: String?
        var stream: String?
        var latency: String?
        var refresh: String?
        var bonjour: String
        var pin: String?
        var family: VRHeadsetFamily?
        var codec: String?
        var battery: String?
        var encryption: String?
        var pairingRequest: String?   // an unpaired headset was refused (pairing off)
    }

    /// Settings > VR "Show live statistics": (title key, value) rows; empty without a connected headset.
    nonisolated static func statLines(_ s: VRStatus?) -> [(String, String)] {
        guard let s, s.headset != nil else { return [] }
        let st = s.stream
        func ms(_ v: Double) -> String { String(format: "%.1f ms", v) }
        var r: [(String, String)] = []
        if let fps = st.game_fps { r.append((String(localized: "Game frame rate"), String(format: "%.1f fps", fps))) }
        if let c = st.submit_copied_ms { r.append((String(localized: "Game frame copy"), ms(c))) }
        if let g = st.compose_gpu_ms { r.append((String(localized: "Compose (Mac GPU)"), ms(g))) }
        r.append((String(localized: "Encode"), ms(st.encode_ms)))
        r.append((String(localized: "Headset decode"), ms(st.decode_ms)))
        if st.pipeline_ms > 0 { r.append((String(localized: "Game frame to network"), ms(st.pipeline_ms))) }
        r.append((String(localized: "Stream bitrate"), String(format: "%.1f / %.0f Mbit/s", st.bitrate_mbps, st.target_mbps)))
        r.append((String(localized: "Frames sent"), String(st.frames)))
        if let i = st.idr_requests { r.append((String(localized: "Lost frames (keyframes requested)"), String(i))) }
        if let u = st.upscaler, !u.isEmpty { r.append((String(localized: "Upscaler"), u == "metalfx" ? "MetalFX" : u)) }
        return r
    }

    nonisolated static func lines(_ s: VRStatus?, available: Bool) -> Lines {
        guard let s else {
            return Lines(host: available ? String(localized: "Not running") : String(localized: "Not included in this build."),
                         bonjour: Host.current().localizedName ?? "")
        }
        var l = Lines(host: String(localized: "Running, port \(String(s.port))"),
                      bonjour: s.bonjour.isEmpty ? String(localized: "Not advertised") : s.bonjour,
                      pin: s.encryption == "pairing" && !s.pin.isEmpty ? s.pin : nil)
        l.encryption = s.encryption == "disabled" ? String(localized: "Off (trusted network only)")
                                                  : String(localized: "On (AES-128, paired headsets)")
        if let r = s.pairing_request, r.refused, l.pin == nil {
            l.pairingRequest = String(localized: "“\(r.name)” tried to connect but is not paired, and pairing is off.")
        }
        if let h = s.headset {
            let hz = Int(h.refresh_hz.rounded())
            // sizes as text: a localized integer would get a grouping separator ("1.440")
            l.headset = String(localized: "\(h.name): \(String(h.eye_width))×\(String(h.eye_height)) per eye, \(hz) Hz")
            l.family = VRHeadsetFamily(name: h.name)
            l.refresh = String(localized: "\(hz) Hz")
            l.codec = h.codec.map { $0 == "h264" ? "H.264" : "HEVC" }
            if let b = h.battery {
                let pct = Int((b.charge * 100).rounded())
                l.battery = b.charging ? String(localized: "\(pct) %, charging") : String(localized: "\(pct) %")
            }
            let rate = String(format: "%.1f", s.stream.bitrate_mbps), target = String(format: "%.0f", s.stream.target_mbps)
            l.stream = String(localized: "\(rate) of \(target) Mbit/s, \(s.stream.frames) frames sent")
            let enc = String(format: "%.1f", s.stream.encode_ms), dec = String(format: "%.1f", s.stream.decode_ms)
            if s.stream.pipeline_ms > 0 {
                let total = String(format: "%.1f", s.stream.pipeline_ms)
                l.latency = String(localized: "game frame to network \(total) ms, encode \(enc) ms, headset decode \(dec) ms")
            } else {
                l.latency = String(localized: "encode \(enc) ms, headset decode \(dec) ms")
            }
        } else {
            l.headset = s.state == "pairing" ? String(localized: "Waiting for a headset (pairing open)")
                                             : String(localized: "Waiting for a headset")
        }
        return l
    }
}

// MARK: - view

/// Settings > VR.
struct VRTab: View {
    @EnvironmentObject var settings: LauncherSettings
    @StateObject private var model = VRPanelModel()

    static let wivrnRelease = URL(string: "https://github.com/WiVRn/WiVRn/releases/tag/v26.9")!

    var body: some View {
        let l = VRPanelModel.lines(model.status, available: VRHost.path != nil)
        Form {
            Section {
                Toggle(isOn: $settings.vrEnabled) {
                    Label2(title: "VR (experimental)",
                           detail: String(localized: "DXMT path only: starts the bundled steamac-vrhost with the VM, which streams games' VR eye images to a standalone headset running the WiVRn v26.9 client on the same network. Games run with the guest's OpenVR runtime; start them in VR mode (below, or from the headset's app list). steamac-vrhost is GPL-3.0 software; see the licences in the app.")
                               + (VRHost.path == nil ? " " + String(localized: "Not included in this build.") : ""),
                           now: false, key: .vrEnabled)
                }
                .disabled(settings.graphicsPath != .dxmt || (VRHost.path == nil && !settings.vrEnabled))
            }

            Section {
                row("VR host", l.host,
                    detail: model.hostRunning ? nil
                        : settings.vrEnabled ? String(localized: "VR is on: the host starts with the VM (DXMT graphics path).")
                        : String(localized: "Turn on VR above, then start the VM."))
                if let h = l.headset { row("Headset", h) }
                if let b = l.battery { row("Headset battery", b) }
                if let s = l.stream { row("Stream", s) }
                if let c = l.codec { row("Codec", c) }
                if let lat = l.latency { row("Latency", lat) }
                if let e = l.encryption { row("Encryption", e) }
                row("Bonjour name", l.bonjour,
                    detail: String(localized: "Headsets on the same network list the Mac under this name; WiVRn can also connect by IP address."))
            } header: { Text("Status") }

            Section {
                if let pin = l.pin {
                    HStack(alignment: .firstTextBaseline) {
                        VStack(alignment: .leading, spacing: 2) {
                            Text("Pairing PIN")
                            Text("Enter it in the WiVRn app on the new headset. The PIN never leaves this network.")
                                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                        Spacer()
                        Text(verbatim: pin).font(.system(.title, design: .monospaced).weight(.semibold)).textSelection(.enabled)
                    }
                    Button("Stop pairing") { model.setPairing(false) }
                } else if let req = l.pairingRequest {
                    Label { Text(verbatim: req) } icon: { Image(systemName: "exclamationmark.triangle") }
                        .foregroundStyle(.orange)
                    Button("Pair this headset") { model.setPairing(true) }
                } else {
                    HStack {
                        Button("Pair a new headset") { model.setPairing(true) }
                            .disabled(!model.hostRunning || model.status?.encryption == "disabled")
                        if !model.hostRunning {
                            Text("Needs the running VR host.").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                Toggle(isOn: $settings.vrPairOnRequest) {
                    Label2(title: "Let new headsets ask to pair",
                           detail: String(localized: "A headset that is not paired yet opens pairing when it connects: its PIN appears here and in the VM window's title. Off: new headsets are refused until you pair them here."),
                           now: false, key: .vrPairOnRequest)
                }
                if model.headsets.isEmpty {
                    Text("No headset paired yet.").foregroundStyle(.secondary)
                } else {
                    ForEach(model.headsets) { h in
                        HStack {
                            VStack(alignment: .leading, spacing: 1) {
                                Text(verbatim: h.count > 1 ? "\(h.name) (\(h.count))" : h.name)
                                Text(lastConnected(h)).font(.caption).foregroundStyle(.secondary)
                                if h.count > 1 {
                                    Text("Paired \(h.count) times (a reinstalled or test client); Unpair removes all of them.")
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                            }
                            Spacer()
                            Button("Unpair") { model.unpair(h.name) }
                        }
                    }
                    if model.headsets.count > 1 { Button("Unpair all") { model.unpair("") } }
                }
                if let e = model.lastError {
                    Label { Text(verbatim: e) } icon: { Image(systemName: "exclamationmark.triangle") }
                        .font(.caption).foregroundStyle(.orange)
                }
            } header: { Text("Pairing") }

            Section {
                Picker(selection: Binding(get: { settings.vrPreset }, set: { if let p = $0 { settings.applyVRPreset(p) } })) {
                    Text("Low").tag(LauncherSettings.VRPreset?.some(.low))
                    Text("Medium").tag(LauncherSettings.VRPreset?.some(.medium))
                    Text("High").tag(LauncherSettings.VRPreset?.some(.high))
                    Text("Ultra").tag(LauncherSettings.VRPreset?.some(.ultra))
                    Text("Custom").tag(LauncherSettings.VRPreset?.none)
                } label: {
                    Label2(title: "Preset",
                           detail: String(localized: "Sets bitrate and sharpening together: Low for a weak Wi-Fi link, Ultra for a strong one. Refresh rate and stream size stay with the headset."),
                           now: false)
                }
                VStack(alignment: .leading) {
                    HStack(alignment: .firstTextBaseline) {
                        Label2(title: "Render resolution",
                               detail: String(localized: "Size the game renders each eye at, relative to what the headset asks for. Below 100 percent the Mac upscales the image before encoding (the game draws fewer pixels); above 100 percent is supersampling."),
                               now: false, key: .vrRenderScale)
                        Spacer()
                        Text(verbatim: "\(Int((settings.vrRenderScale * 100).rounded())) %").foregroundStyle(.secondary).monospacedDigit()
                    }
                    Slider(value: $settings.vrRenderScale, in: LauncherSettings.vrRenderScaleRange, step: 0.05)
                }
                Picker(selection: $settings.vrUpscaler) {
                    Text(verbatim: "MetalFX").tag(LauncherSettings.VRUpscaler.metalfx)
                    Text("Bilinear").tag(LauncherSettings.VRUpscaler.bilinear)
                } label: {
                    Label2(title: "Upscaler",
                           detail: String(localized: "Used when the render resolution is below 100 percent. MetalFX (spatial) keeps edges sharper but competes with the game for the Mac's GPU (12-15 ms per frame while VRChat ran, against 4-5 ms for bilinear). Only for games limited by GPU pixels; VRChat was not faster at a lower render resolution. Temporal MetalFX needs motion vectors and depth, which OpenVR games do not hand over."),
                           now: false, key: .vrUpscaler)
                }
                .disabled(settings.vrRenderScale >= 1)
                VStack(alignment: .leading) {
                    HStack(alignment: .firstTextBaseline) {
                        Label2(title: "Sharpening",
                               detail: String(localized: "Sharpens the image while it is prepared for encoding; helps an upscaled or softly encoded picture. No measurable cost."),
                               now: false, key: .vrSharpness)
                        Spacer()
                        Text(verbatim: "\(Int((settings.vrSharpness * 100).rounded())) %").foregroundStyle(.secondary).monospacedDigit()
                    }
                    Slider(value: $settings.vrSharpness, in: 0...1, step: 0.05)
                }
                Toggle(isOn: $settings.vrForceSRGB) {
                    Label2(title: "Treat game images as sRGB",
                           detail: String(localized: "For a game that looks washed out or too bright in the headset: its images hold sRGB colours without saying so. Leave off otherwise."),
                           now: false, key: .vrForceSRGB)
                }
            } header: { Text("Quality") }

            Section {
                Picker(selection: $settings.vrCodec) {
                    Text("Automatic").tag(LauncherSettings.VRCodec.automatic)
                    Text(verbatim: "HEVC").tag(LauncherSettings.VRCodec.hevc)
                    Text(verbatim: "H.264").tag(LauncherSettings.VRCodec.h264)
                } label: {
                    Label2(title: "Codec",
                           detail: String(localized: "Automatic uses HEVC when the headset supports it (sharper at the same bitrate). H.264 can help a headset whose HEVC decoder struggles."),
                           now: false, key: .vrCodec)
                }
                Stepper(value: $settings.vrBitrateMbps, in: LauncherSettings.vrBitrateRange, step: 5) {
                    HStack(alignment: .firstTextBaseline) {
                        Label2(title: "Bitrate",
                               detail: String(localized: "Total for both eyes. Higher is sharper and needs a faster Wi-Fi link; the headset can lower it while streaming. Applies at once to a running VR host."),
                               now: true, key: .vrBitrateMbps)
                        Spacer()
                        Text("\(settings.vrBitrateMbps) Mbit/s").foregroundStyle(.secondary).monospacedDigit()
                    }
                }
                .onChange(of: settings.vrBitrateMbps) { _, mbps in model.setBitrate(mbps) }
                Picker(selection: $settings.vrKeyframeInterval) {
                    Text("When needed").tag(0.0)
                    ForEach(LauncherSettings.vrKeyframeIntervals.dropFirst(), id: \.self) { s in
                        Text("Every \(Int(s)) s").tag(s)
                    }
                } label: {
                    Label2(title: "Keyframe interval",
                           detail: String(localized: "When needed: a full frame only when the headset reports a lost one (lowest bitrate). A fixed interval recovers from damage the headset did not report, at the cost of a bitrate spike each time."),
                           now: false, key: .vrKeyframeInterval)
                }
            } header: { Text("Video codec") }

            Section {
                Picker(selection: $settings.vrEyeSize) {
                    Text("Headset default").tag("")
                    ForEach(eyeSizes, id: \.self) { s in
                        Text(verbatim: s.replacingOccurrences(of: "x", with: " × ")).tag(s)
                    }
                } label: {
                    Label2(title: "Resolution per eye",
                           detail: String(localized: "Size of the encoded image for each eye. Headset default uses what the headset asks for; a smaller size encodes faster, which high refresh rates need."),
                           now: false, key: .vrEyeSize)
                }
                row("Refresh rate", l.refresh ?? String(localized: "Chosen by the headset"),
                    detail: String(localized: "Set in the WiVRn app on the headset (as are its resolution scale and microphone); the host encodes at that rate."))
                Toggle(isOn: $settings.vrTCPOnly) {
                    Label2(title: "TCP only",
                           detail: String(localized: "Sends the video over TCP instead of UDP, for networks that drop UDP. Adds latency when packets are lost."),
                           now: false, key: .vrTCPOnly)
                }
            } header: { Text("Stream") }

            Section {
                HStack(alignment: .firstTextBaseline) {
                    Label2(title: "Steam app ID",
                           detail: String(localized: "The game “Start in VR” starts in its VR mode (steam://launch/ID/vr; a normal start may pick desktop mode). The headset's WiVRn app also lists it under its apps, from the next start. VRChat is 438100."),
                           now: true, key: .vrGameAppID)
                    Spacer()
                    TextField("ID", value: $settings.vrGameAppID, format: .number.grouping(.never))
                        .frame(width: 90).multilineTextAlignment(.trailing)
                }
                HStack {
                    Button("Start in VR") { VRHost.launchInVR(settings.vrGameAppID) }
                        .disabled(VRHost.guestSend == nil || !model.hostRunning || settings.vrGameAppID <= 0)
                    if VRHost.guestSend == nil || !model.hostRunning {
                        Text("Needs the VM running with VR on.").font(.caption).foregroundStyle(.secondary)
                    } else if model.status?.headset == nil {
                        Text("Connect the headset first, then start the game.").font(.caption).foregroundStyle(.secondary)
                    }
                }
                Toggle(isOn: $settings.vrHeadsetAudio) {
                    Label2(title: "Game audio in the headset",
                           detail: String(localized: "While a headset is connected, the guest's sound plays in the headset and its microphone is the guest's input (the microphone opens only while a game records). Off: sound stays on the Mac."),
                           now: false, key: .vrHeadsetAudio)
                }
            } header: { Text("Game") }

            Section {
                Toggle("Show live statistics", isOn: $settings.vrShowStats)
                if settings.vrShowStats {
                    ForEach(VRPanelModel.statLines(model.status), id: \.0) { title, value in
                        HStack {
                            Text(verbatim: title)
                            Spacer()
                            Text(verbatim: value).foregroundStyle(.secondary).monospacedDigit()
                        }
                    }
                }
            } header: { Text("Statistics") }

            Section {
                hint(.questPico, l.family, title: "Meta Quest and Pico",
                     text: String(localized: "Install WiVRn v26.9 (WiVRn-release.apk from the release page) with adb in developer mode, and turn off automatic updates for WiVRn: a newer client does not connect."))
                hint(.steamFrame, l.family, title: "Steam Frame",
                     text: String(localized: "Build WiVRn's Linux client from the v26.9 tag (client on, server off) and connect to this Mac by IP address, port 9757."))
                Link("WiVRn v26.9 release", destination: Self.wivrnRelease)
            } header: { Text("Headset setup") } footer: {
                Text("steamac-vrhost is GPL-3.0 software based on WiVRn; its licence and source are listed in the app's licences.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .onAppear { model.start() }
        .onDisappear { model.stop() }
    }

    /// The presets plus a size set from the command line or `defaults`.
    private var eyeSizes: [String] {
        LauncherSettings.vrEyeSizes + (settings.vrEyeSize.isEmpty || LauncherSettings.vrEyeSizes.contains(settings.vrEyeSize)
                                       ? [] : [settings.vrEyeSize])
    }

    private func row(_ title: LocalizedStringKey, _ value: String, detail: String? = nil) -> some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                if let detail { Text(verbatim: detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true) }
            }
            Spacer()
            Text(verbatim: value).foregroundStyle(.secondary).multilineTextAlignment(.trailing).textSelection(.enabled)
        }
    }

    private func hint(_ family: VRHeadsetFamily, _ current: VRHeadsetFamily?, title: LocalizedStringKey, text: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(title)
                if family == current { Text("connected").font(.caption).foregroundStyle(.green) }
            }
            Text(verbatim: text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    private func lastConnected(_ h: VRPairedHeadset) -> String {
        guard h.last_connection > 0 else { return String(localized: "Never connected") }
        let when = Date(timeIntervalSince1970: TimeInterval(h.last_connection)).formatted(date: .abbreviated, time: .shortened)
        return String(localized: "Last connected \(when)")
    }
}

// MARK: - self-test

/// The panel's part of `--selftest-vr` (VRSelfTest): text derivation from fixed replies, and the control
/// socket of a running host (status, pairing on/off with a fresh PIN, the paired list through the socket
/// and through the executable). Each function returns its number of failures.
enum VRPanelSelfTest {
    private static func check(_ ok: Bool, _ what: String, _ failures: inout Int) {
        if !ok { failures += 1; log("selftest-vr: FAIL panel: \(what)") }
    }

    static func text() -> Int {
        var f = 0
        let connected = #"{"v":1,"state":"connected","port":9757,"bonjour":"studio","encryption":"enabled","pin":"","#
            + #""headset":{"name":"Meta Quest 3","eye_width":1440,"eye_height":1584,"render_width":2064,"render_height":2208,"#
            + #""refresh_hz":90.0,"refresh_rates":[72,90,120]},"stream":{"frames":900,"bitrate_mbps":48.2,"target_mbps":50.0,"#
            + #""encode_ms":6.10,"decode_ms":7.25,"pipeline_ms":11.40}}"#
        let pairing = #"{"v":1,"state":"pairing","port":9757,"bonjour":"","encryption":"pairing","pin":"123456","headset":null,"#
            + #""stream":{"frames":0,"bitrate_mbps":0.0,"target_mbps":50.0,"encode_ms":0.00,"decode_ms":0.00,"pipeline_ms":0.00}}"#
        guard let c = try? JSONDecoder().decode(VRStatus.self, from: Data(connected.utf8)),
              let p = try? JSONDecoder().decode(VRStatus.self, from: Data(pairing.utf8)) else {
            check(false, "status replies do not decode", &f)
            return f
        }
        let lc = VRPanelModel.lines(c, available: true)
        check(lc.headset?.contains("Meta Quest 3") == true && lc.headset?.contains("1440×1584") == true && lc.headset?.contains("90") == true,
              "connected headset line: \(lc.headset ?? "-")", &f)
        check(lc.stream?.contains("48.2") == true && lc.stream?.contains("900") == true, "stream line: \(lc.stream ?? "-")", &f)
        check(lc.latency?.contains("11.4") == true && lc.latency?.contains("6.1") == true, "latency line: \(lc.latency ?? "-")", &f)
        check(lc.host.contains("9757"), "host line: \(lc.host)", &f)
        check(lc.refresh?.contains("90") == true && lc.pin == nil && lc.family == .questPico && lc.bonjour == "studio",
              "connected: refresh/pin/family/bonjour", &f)
        let lp = VRPanelModel.lines(p, available: true)
        check(lp.pin == "123456" && lp.stream == nil && lp.refresh == nil && lp.family == nil && lp.headset != nil,
              "pairing: PIN shown, no stream lines", &f)
        check(VRPanelModel.hostError("unknown request") != "unknown request" && VRPanelModel.hostError("x y") == "x y",
              "host errors: known ones replaced, others kept", &f)
        let off = VRPanelModel.lines(nil, available: true), absent = VRPanelModel.lines(nil, available: false)
        check(off.pin == nil && off.headset == nil && off.host != absent.host, "no host: running and not-built differ", &f)
        check(VRHeadsetFamily(name: "PICO 4 Ultra") == .questPico && VRHeadsetFamily(name: "Steam Frame") == .steamFrame
              && VRHeadsetFamily(name: "Lynx R1") == .other, "headset families", &f)
        let rows = "{\"name\":\"Quest \\\"A\\\"\",\"last_connection\":1760000000}\n{\"name\":\"Pico\",\"last_connection\":0}\n"
        let list = rows.split(separator: "\n").compactMap { try? JSONDecoder().decode(VRPairedHeadset.self, from: Data($0.utf8)) }
        check(list.map(\.name) == ["Quest \"A\"", "Pico"] && list[0].last_connection == 1_760_000_000, "--list-headsets --json rows", &f)
        // since vr-polish: codec, battery, a refused pairing request, grouped paired list
        let polished = #"{"v":1,"state":"connected","port":9757,"bonjour":"","encryption":"enabled","pin":"","#
            + #""headset":{"name":"Oculus Quest 3","eye_width":1832,"eye_height":1920,"refresh_hz":90.0,"refresh_rates":[72,90,120],"#
            + #""codec":"h264","battery":{"charge":0.81,"charging":true}},"pairing_request":{"name":"Pico 4","at":1,"refused":true},"#
            + #""app_request":{"id":"438100","seq":2},"stream":{"frames":1,"bitrate_mbps":1.0,"target_mbps":50.0,"encode_ms":1.0,"#
            + #""decode_ms":1.0,"pipeline_ms":0}}"#
        if let ps = try? JSONDecoder().decode(VRStatus.self, from: Data(polished.utf8)) {
            let l = VRPanelModel.lines(ps, available: true)
            check(l.codec == "H.264" && l.battery?.contains("81") == true && l.pairingRequest?.contains("Pico 4") == true
                  && l.encryption != nil && ps.app_request?.seq == 2, "polished status: \(l)", &f)
        } else {
            check(false, "polished status does not decode", &f)
        }
        check(lc.codec == nil && lc.battery == nil && lc.pairingRequest == nil, "older host: no codec/battery/request lines", &f)
        let stats = VRPanelModel.statLines(c).map(\.1)
        check(stats.contains("11.4 ms") && stats.contains("900") && VRPanelModel.statLines(p).isEmpty, "statistics rows: \(stats)", &f)
        let dup = [VRPairedHeadset(name: "Unknown headset", last_connection: 5), VRPairedHeadset(name: "Oculus Quest 3", last_connection: 9),
                   VRPairedHeadset(name: "Unknown headset", last_connection: 7)]
        let g = VRPairedHeadset.grouped(dup)
        check(g.map(\.name) == ["Oculus Quest 3", "Unknown headset"] && g.map(\.count) == [1, 2] && g[1].last_connection == 7,
              "grouped paired list: \(g)", &f)
        return f
    }

    static func control(socket: String, executable: String, round: Int) -> Int {
        var f = 0
        func status(_ request: String) -> VRStatus? {
            // the host binds the socket right after it starts listening: allow it a moment
            for _ in 0..<50 {
                if let d = VRControl.request(request, path: socket) { return try? JSONDecoder().decode(VRStatus.self, from: d) }
                usleep(20_000)
            }
            return nil
        }
        guard let s = status("status") else {
            check(false, "round \(round): no status from the control socket \(socket)", &f)
            return f
        }
        // nothing paired in the scratch store: the host starts in pairing mode with a PIN
        check(s.v == 1 && s.state == "pairing" && s.encryption == "pairing" && s.port > 0 && s.headset == nil,
              "round \(round): status \(s)", &f)
        check(s.pin.count == 6 && s.pin.allSatisfy(\.isNumber), "round \(round): PIN '\(s.pin)'", &f)
        check(VRPanelModel.lines(s, available: true).pin == s.pin, "round \(round): panel does not show the PIN", &f)
        let offStatus = status("pair off")
        check(offStatus?.encryption == "enabled" && offStatus?.pin == "" && offStatus?.state == "waiting",
              "round \(round): pair off: \(String(describing: offStatus))", &f)
        check(VRPanelModel.cliHeadsets(executable) == [], "round \(round): --list-headsets --json of an empty store", &f)
        let on = status("pair on")
        check(on?.encryption == "pairing" && on?.pin.count == 6, "round \(round): pair on: \(String(describing: on))", &f)
        // the PIN file follows (what --show-pin prints)
        check(VRPanelModel.run(executable, ["--show-pin"])?.trimmingCharacters(in: .whitespacesAndNewlines) == on?.pin,
              "round \(round): --show-pin differs from the socket's PIN", &f)
        let list = VRControl.request("headsets", path: socket).map { String(decoding: $0, as: UTF8.self) } ?? ""
        check(list.contains("\"headsets\":[]"), "round \(round): headsets: \(list)", &f)
        // the link host's live statistics (Settings > VR "Show live statistics")
        check(s.stream.game_fps != nil && s.stream.compose_gpu_ms != nil && s.stream.upscaler == "" && s.stream.idr_requests == 0,
              "round \(round): stream statistics \(s.stream)", &f)
        let rate = status("bitrate 30")
        check(rate?.stream.target_mbps == 30, "round \(round): bitrate 30: \(String(describing: rate))", &f)
        check(status("bitrate 0") == nil, "round \(round): bitrate 0 accepted", &f) // an error reply, not a status
        let bad = VRControl.request("frobnicate", path: socket).map { String(decoding: $0, as: UTF8.self) } ?? ""
        check(bad.contains("\"error\""), "round \(round): unknown request: \(bad)", &f)
        log("selftest-vr: round \(round): control socket ok (port \(s.port), pairing off/on, PIN file, lists)")
        return f
    }
}
