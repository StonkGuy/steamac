import CKrun
import Darwin
import Foundation

/// VR (experimental, Settings > VR, `--vr`): the VM process starts `steamac-vrhost`, a separate
/// GPL-3.0-or-later executable in the app (Contents/MacOS; WiVRn-compatible headset server, compositor and
/// encoder: host/vrhost, docs/vr.md). This process never links it; the coupling is the VR link of
/// docs/vr-interfaces.md section 3: an AF_UNIX SOCK_DGRAM socketpair (the host's end passed as --link-fd)
/// and a bootstrap name for the swapchain IOSurface ports (--mach-name). Our end is announced to the wmtr
/// executor (libwmtr_host, loaded by libkrun in this process) through WMTR_VR_FD / WMTR_VR_MACH_NAME, so
/// DXMT's WMTVRSubmit (call 146) reaches the host. The host ends when this process closes its end: libkrun
/// `_exit()`s the process when the VM stops, which closes it, so the host goes down with the VM even
/// without stop(). Off (the default): nothing is created and the variables are cleared.
enum VRHost {
    static let executable = "steamac-vrhost"
    static let envKeys = ["WMTR_VR_FD", "WMTR_VR_MACH_NAME"]

    struct Running {
        let pid: pid_t
        let fd: Int32          // our end of the link (kept open for the VM's lifetime)
        let machName: String
    }
    nonisolated(unsafe) private(set) static var running: Running?

    /// The bundled host executable; STEAMAC_VRHOST overrides it (development builds, self-test).
    static var path: String? {
        if let p = ProcessInfo.processInfo.environment["STEAMAC_VRHOST"], !p.isEmpty {
            return FileManager.default.isExecutableFile(atPath: p) ? p : nil
        }
        guard let p = Bundle.main.path(forAuxiliaryExecutable: executable) else { return nil }
        return FileManager.default.isExecutableFile(atPath: p) ? p : nil
    }

    /// English diagnostic for logs and command-line errors; nil if this boot can use VR.
    static func unavailableReason(_ o: Options) -> String? {
        if path == nil { return "not included in this build (\(executable))" }
        if o.graphicsPath != .dxmt { return "needs the DXMT graphics path" }
        return nil
    }

    static var logPath: String {
        NSHomeDirectory() + "/Library/Logs/" + LauncherSettings.defaultDomain + "/steamac-vrhost.log"
    }

    /// Called once per boot before libkrun creates the wmtr context (VM.start). Never fatal: VR stays off
    /// with a log line when the host cannot start.
    /// With a virtio-snd device (`sound`) it also creates the VR audio region (docs/vr-interfaces.md section 6),
    /// attaches it to libkrun and hands it to the host (--audio-fd): the guest's audio follows a connected
    /// headset and is the Mac's again whenever none is connected (the host clears the region's headset word).
    static func apply(_ o: Options, ctx: UInt32, sound: Bool) {
        for k in envKeys { unsetenv(k) }
        guard o.vr else { return }
        if let reason = unavailableReason(o) {
            log("vr: off, \(reason)")
            return
        }
        let audioFD = sound ? audioRegion(ctx: ctx) : -1
        defer { if audioFD >= 0 { close(audioFD) } } // mapped by libkrun, inherited by the host
        do {
            let r = try start(extraArgs: hostArgs(o), logPath: logPath, audioFD: audioFD)
            setenv("WMTR_VR_FD", String(r.fd), 1)
            setenv("WMTR_VR_MACH_NAME", r.machName, 1)
            log("vr: steamac-vrhost pid \(r.pid), link fd \(r.fd), mach name \(r.machName), log \(logPath)")
        } catch {
            log("vr: off, \(error)")
        }
    }

    /// What Settings > VR passes to the host of this boot: the control socket the panel reads
    /// (VRControl), the total bitrate and, unless the headset's own size is used, the size per eye; the codec
    /// unless automatic; pairing on request; the game for the headset's app list (WiVRn's "Start app").
    static func hostArgs(_ o: Options, controlSocket: String = VRControl.socketPath) -> [String] {
        var a = ["--control-socket", controlSocket, "--bitrate", String(o.vrBitrateMbps * 1_000_000)]
        if !o.vrEyeSize.isEmpty { a += ["--size", o.vrEyeSize] }
        if o.vrCodec != .automatic { a += ["--codec", o.vrCodec.rawValue] }
        if o.vrPairOnRequest { a.append("--pair-on-request") }
        if o.vrRenderScale != 1 {
            a += ["--render-scale", String(format: "%.2f", o.vrRenderScale)]
            if o.vrUpscaler != .metalfx { a += ["--upscaler", o.vrUpscaler.rawValue] }
        }
        if o.vrSharpness > 0 { a += ["--sharpness", String(format: "%.2f", o.vrSharpness)] }
        if o.vrForceSRGB { a.append("--force-srgb") }
        if o.vrKeyframeInterval > 0 { a += ["--keyframe-interval", String(format: "%g", o.vrKeyframeInterval)] }
        if o.vrTCPOnly { a.append("--tcp-only") }
        if o.vrGameAppID > 0 { a += ["--app", "\(o.vrGameAppID)=\(gameName(o.vrGameAppID, known: o.vrGameName))"] }
        return a
    }

    /// The name the headset's app list shows (English: the headset has no access to the launcher's language).
    static func gameName(_ appID: Int, known: String?) -> String {
        if let n = known, !n.isEmpty { return n }
        return appID == LauncherSettings.vrDefaultGameAppID ? "VRChat" : "Steam app \(appID)"
    }

    /// Host -> guest request line (fx.progress), set by main.swift for a VR boot; nil = no running guest agent.
    nonisolated(unsafe) static var guestSend: ((String) -> Bool)?

    /// Starts a Steam game in its VR launch mode in the guest (fx-progress-agent `launch-vr`, which runs
    /// `steam -ifrunning steam://launch/<id>/vr`). False when the VM is not running with VR.
    @discardableResult
    static func launchInVR(_ appID: Int) -> Bool {
        guard appID > 0, let send = guestSend else { return false }
        log("vr: start app \(appID) in VR")
        return send("launch-vr \(appID)")
    }

    /// A new VR audio region attached to libkrun (local output silenced while a headset plays), or -1 with
    /// a log line: VR video still works, the guest's audio then stays on the Mac.
    static func audioRegion(ctx: UInt32) -> Int32 {
        let fd = steamac_vra_create()
        guard fd >= 0 else {
            log("vr: no headset audio, region: \(String(cString: strerror(-fd)))")
            return -1
        }
        let rc = krun_snd_set_vr_audio(ctx, fd, UInt32(KRUN_SND_VR_MUTE_LOCAL))
        guard rc == 0 else {
            log("vr: no headset audio, krun_snd_set_vr_audio: \(rc)")
            close(fd)
            return -1
        }
        return fd
    }

    /// Creates the link, spawns the host with its end, and returns our end (FD_CLOEXEC: no other child
    /// inherits it, so the host sees end-of-file exactly when this process lets go of it).
    @discardableResult
    static func start(extraArgs: [String], logPath: String?, audioFD: Int32 = -1) throws -> Running {
        guard let exe = path else { throw OptionError("\(executable) not found") }
        var sv: [Int32] = [-1, -1]
        guard socketpair(AF_UNIX, SOCK_DGRAM, 0, &sv) == 0 else {
            throw OptionError("socketpair: \(String(cString: strerror(errno)))")
        }
        _ = fcntl(sv[0], F_SETFD, FD_CLOEXEC)
        // VRL_SOCKET_BUFFER (vrlink.h): per-frame timing/pose packets wait here until the guest calls in
        var space: Int32 = 256 * 1024
        setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &space, socklen_t(MemoryLayout<Int32>.size))
        let machName = "\(LauncherSettings.defaultDomain).vrhost.\(getpid())"

        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_addinherit_np(&actions, sv[1])
        if audioFD >= 0 { posix_spawn_file_actions_addinherit_np(&actions, audioFD) }
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
        if let logPath {
            try? FileManager.default.createDirectory(atPath: (logPath as NSString).deletingLastPathComponent,
                                                     withIntermediateDirectories: true)
            posix_spawn_file_actions_addopen(&actions, 1, logPath, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
            posix_spawn_file_actions_adddup2(&actions, 1, 2)
        } else {
            posix_spawn_file_actions_addinherit_np(&actions, 1)
            posix_spawn_file_actions_addinherit_np(&actions, 2)
        }
        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        var defaults = sigset_t(), noMask = sigset_t()
        sigfillset(&defaults); sigemptyset(&noMask)
        posix_spawnattr_setsigdefault(&attr, &defaults)
        posix_spawnattr_setsigmask(&attr, &noMask)
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK))

        let args = [exe, "--link-fd", String(sv[1]), "--mach-name", machName]
            + (audioFD >= 0 ? ["--audio-fd", String(audioFD)] : []) + extraArgs
        var cArgs = args.map { strdup($0) } + [nil]
        defer { for p in cArgs { free(p) } }
        var pid: pid_t = 0
        let rc = posix_spawn(&pid, exe, &actions, &attr, &cArgs, environ)
        close(sv[1])
        guard rc == 0 else {
            close(sv[0])
            throw OptionError("posix_spawn \(exe): \(String(cString: strerror(rc)))")
        }
        let r = Running(pid: pid, fd: sv[0], machName: machName)
        running = r
        return r
    }

    /// Ends the host: closes our end (the host stops on end-of-file), then SIGTERM, then SIGKILL after
    /// `timeout`. Returns how the child ended. Only for our own exit paths and the self-test; a VM stop
    /// through libkrun's `_exit()` closes the end implicitly.
    @discardableResult
    static func stop(timeout: TimeInterval = 2) -> String {
        guard let r = running else { return "not running" }
        running = nil
        for k in envKeys { unsetenv(k) }
        close(r.fd)
        if let how = wait(r.pid, for: timeout) { return "exited on link close (\(how))" }
        kill(r.pid, SIGTERM)
        if let how = wait(r.pid, for: timeout) { return "exited on SIGTERM (\(how))" }
        kill(r.pid, SIGKILL)
        var st: Int32 = 0
        waitpid(r.pid, &st, 0)
        return "killed"
    }

    private static func wait(_ pid: pid_t, for timeout: TimeInterval) -> String? {
        let end = Date().addingTimeInterval(timeout)
        repeat {
            var st: Int32 = 0
            if waitpid(pid, &st, WNOHANG) == pid {
                return (st & 0x7f) == 0 ? "status \((st >> 8) & 0xff)" : "signal \(st & 0x7f)"
            }
            usleep(20_000)
        } while Date() < end
        return nil
    }
}

/// `--selftest-vr`: the spawn/teardown path without a VM or headset. Off adds nothing; on spawns the host
/// (bundled, or STEAMAC_VRHOST) with its link end, exports WMTR_VR_FD/WMTR_VR_MACH_NAME for libwmtr_host,
/// receives the host's VRL_HELLO on our end (struct vrl_hello, host/vrhost/include/vrlink.h) with the
/// child's pid, and checks that closing our end alone ends the host (what a VM stop does); a second run
/// checks that the host's port is free again.
enum VRSelfTest {
    /// The host maps the region and beats on it with no headset connected: headset word 0, so libkrun
    /// keeps the guest's audio on the Mac.
    static func audio(fd: Int32) -> Int {
        let n = Int(STEAMAC_VRA_REGION_BYTES)
        guard let p = mmap(nil, n, PROT_READ, MAP_SHARED, fd, 0), p != MAP_FAILED else {
            log("selftest-vr: FAIL audio region mmap"); return 1
        }
        defer { munmap(p, n) }
        let r = p.assumingMemoryBound(to: vra_region.self)
        var bad = 0
        if vra_region_valid(r) == 0 { log("selftest-vr: FAIL audio region not valid"); bad += 1 }
        let b0 = r.pointee.beat
        var beat = b0
        for _ in 0..<50 where beat == b0 {
            usleep(20_000)
            OSMemoryBarrier()
            beat = r.pointee.beat
        }
        if beat == b0 { log("selftest-vr: FAIL host does not beat on the audio region"); bad += 1 }
        if r.pointee.headset != 0 { log("selftest-vr: FAIL headset word \(r.pointee.headset) with no headset"); bad += 1 }
        if bad == 0 { log("selftest-vr: audio region ok (beat \(b0) -> \(beat), no headset: Mac output)") }
        return bad
    }

    static func run(_ resolved: Options) -> Never {
        var failures = 0
        func check(_ ok: Bool, _ what: String) { if !ok { failures += 1; log("selftest-vr: FAIL \(what)") } }

        var off = resolved
        off.vr = false
        setenv("WMTR_VR_FD", "99", 1)   // left over from a shell: must not survive VR off
        VRHost.apply(off, ctx: 0, sound: true)
        check(getenv("WMTR_VR_FD") == nil && getenv("WMTR_VR_MACH_NAME") == nil, "VR off: link environment left set")
        check(VRHost.running == nil, "VR off: host started")
        check(Options().vr == false, "VR must default off")
        // Settings > VR: on maps to steamac.vr=1, off adds no key; the encoder settings reach the host's
        // command line, the headset's own size by default.
        check(Options.vrArg(true)?.key == "steamac.vr" && Options.vrArg(true)?.value == "1", "VR on must map to steamac.vr=1")
        check(Options.vrArg(false) == nil, "VR off must add no kernel key")
        var enc = Options()
        check(VRHost.hostArgs(enc, controlSocket: "/s") == ["--control-socket", "/s", "--bitrate", "50000000",
                                                            "--pair-on-request", "--app", "438100=VRChat"],
              "default host arguments: \(VRHost.hostArgs(enc, controlSocket: "/s"))")
        enc.vrBitrateMbps = 80
        enc.vrEyeSize = "1440x1584"
        enc.vrCodec = .h264
        enc.vrPairOnRequest = false
        enc.vrGameAppID = 620
        enc.vrGameName = "Portal 2"
        check(VRHost.hostArgs(enc, controlSocket: "/s").suffix(8) == ["--bitrate", "80000000", "--size", "1440x1584",
                                                                      "--codec", "h264", "--app", "620=Portal 2"],
              "host arguments with bitrate, size, codec, game: \(VRHost.hostArgs(enc, controlSocket: "/s"))")
        enc.vrRenderScale = 0.6
        enc.vrUpscaler = .bilinear
        enc.vrSharpness = 0.25
        enc.vrForceSRGB = true
        enc.vrKeyframeInterval = 2
        enc.vrTCPOnly = true
        let q = Array(VRHost.hostArgs(enc, controlSocket: "/s").drop { $0 != "--render-scale" })
        check(q == ["--render-scale", "0.60", "--upscaler", "bilinear", "--sharpness", "0.25", "--force-srgb",
                    "--keyframe-interval", "2", "--tcp-only", "--app", "620=Portal 2"],
              "quality/codec/stream arguments: \(q)")
        let presets = LauncherSettings.VRPreset.allCases
        check(presets.allSatisfy { $0.renderScale == 1 } && presets.map(\.bitrateMbps) == [30, 40, 50, 80] && LauncherSettings.VRPreset.high.bitrateMbps == LauncherSettings.vrDefaultBitrateMbps
              && presets.allSatisfy { LauncherSettings.vrRenderScaleRange.contains($0.renderScale)
                                      && LauncherSettings.vrBitrateRange.contains($0.bitrateMbps) }, "VR presets")
        enc.vrGameAppID = 0
        check(!VRHost.hostArgs(enc).contains("--app") && VRHost.gameName(7, known: nil) == "Steam app 7", "no game / unnamed game")
        check(VRHost.launchInVR(438100) == false, "launch in VR without a guest must be refused")
        var sent: [String] = []
        VRHost.guestSend = { sent.append($0); return true }
        check(VRHost.launchInVR(438100) && !VRHost.launchInVR(0) && sent == ["launch-vr 438100"], "launch-vr request: \(sent)")
        VRHost.guestSend = nil
        // the relay: first reply = baseline, a newer start request starts the game, the PIN only while pairing
        func reply(_ enc: String, _ pin: String, _ seq: UInt64?) -> VRRelay.Reply {
            VRRelay.Reply(encryption: enc, pin: pin, app_request: seq.map { .init(id: "438100", seq: $0) })
        }
        let r0 = VRRelay.actions(reply("enabled", "", 3), lastSeq: nil)
        let r1 = VRRelay.actions(reply("pairing", "123456", 4), lastSeq: 3)
        let r2 = VRRelay.actions(nil, lastSeq: 4)
        check(r0.start == nil && r0.seq == 3 && r0.pin == nil && r1.start == 438100 && r1.seq == 4 && r1.pin == "123456"
              && r2.start == nil && r2.seq == 4 && r2.pin == nil, "relay actions: \(r0) \(r1) \(r2)")
        check(LauncherSettings.vrEyeSizes.allSatisfy(LauncherSettings.vrEyeSizeValid)
              && !["", "1440", "1441x1584", "8x8", "axb"].contains(where: LauncherSettings.vrEyeSizeValid), "eye size validation")
        failures += VRPanelSelfTest.text()

        guard VRHost.path != nil else {
            log("selftest-vr: FAIL \(VRHost.executable) not in this build (set STEAMAC_VRHOST for a development build)")
            exit(1)
        }
        // The host of this test keeps its pairing store and PIN file under a scratch HOME (never the
        // user's paired headsets); its control socket is next to them.
        let scratch = "/tmp/steamac-selftest-vr.\(getpid())"
        try? FileManager.default.createDirectory(atPath: scratch, withIntermediateDirectories: true)
        setenv("HOME", scratch, 1)
        let sock = scratch + "/control.sock"
        for round in 1...2 {
            do {
                // no Bonjour, fixed loopback-free port 0: the self-test must not show up on headsets
                // round 2 also hands over a VR audio region (what apply() does with a sound device, minus libkrun)
                let audioFD: Int32 = round == 2 ? steamac_vra_create() : -1
                check(round == 1 || audioFD >= 0, "round 2: steamac_vra_create \(audioFD)")
                defer { if audioFD >= 0 { close(audioFD) } }
                let r = try VRHost.start(extraArgs: ["--no-advertise", "--port", "0", "--control-socket", sock], logPath: nil,
                                         audioFD: audioFD)
                setenv("WMTR_VR_FD", String(r.fd), 1)
                setenv("WMTR_VR_MACH_NAME", r.machName, 1)
                check(fcntl(r.fd, F_GETFD) & FD_CLOEXEC != 0, "our link end is inheritable")
                var buf = [UInt8](repeating: 0, count: 2048)
                var pfd = pollfd(fd: r.fd, events: Int16(POLLIN), revents: 0)
                let n = poll(&pfd, 1, 5000) == 1 ? recv(r.fd, &buf, buf.count, 0) : -1
                // struct vrl_hello { struct vrl_header {u32 type, size}; u32 version; u32 pid; }
                let words = buf.withUnsafeBytes { Array($0.bindMemory(to: UInt32.self).prefix(4)) }
                check(n >= 16 && words[0] == 1 && words[3] == UInt32(r.pid),
                      "round \(round): no VRL_HELLO from the host (got \(n) bytes, type \(words[0]), pid \(words[3]) vs \(r.pid))")
                check(kill(r.pid, 0) == 0, "round \(round): host not running")
                if audioFD >= 0 { failures += audio(fd: audioFD) }
                failures += VRPanelSelfTest.control(socket: sock, executable: VRHost.path!, round: round)
                let how = VRHost.stop()
                log("selftest-vr: round \(round): host pid \(r.pid), hello version \(words[2]), \(how)")
                check(how.hasPrefix("exited on link close"), "round \(round): host did not end when the link closed (\(how))")
                check(getenv("WMTR_VR_FD") == nil, "round \(round): link environment left set after stop")
            } catch {
                check(false, "round \(round): \(error)")
            }
        }
        try? FileManager.default.removeItem(atPath: scratch)
        log(failures == 0 ? "selftest-vr: PASS" : "selftest-vr: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}

/// A VR boot's link between the host's control socket and the rest of the launcher, without Settings open:
/// every 2 s (VR boots only; nothing runs otherwise) it reads the host's status and
/// * starts the game the headset picked in its WiVRn app list (`app_request` seq moved on) in the guest;
/// * reports the pairing PIN while pairing is open (window subtitle), so a headset that asks for a PIN finds
///   it on the VM window and not only in Settings > VR.
@MainActor
final class VRRelay {
    struct Reply: Decodable {
        struct AppRequest: Decodable { var id: String; var seq: UInt64 }
        var encryption: String
        var pin: String
        var app_request: AppRequest?
    }

    /// What one status reply asks for: the app to start (a new request since `lastSeq`) and the PIN to show.
    nonisolated static func actions(_ r: Reply?, lastSeq: UInt64?) -> (start: Int?, seq: UInt64?, pin: String?) {
        guard let r else { return (nil, lastSeq, nil) }
        let pin = r.encryption == "pairing" && !r.pin.isEmpty ? r.pin : nil
        guard let a = r.app_request else { return (nil, lastSeq, pin) }
        // the first reply only sets the baseline: a request from before this launcher run is not repeated
        let start = lastSeq != nil && a.seq > lastSeq! ? Int(a.id) : nil
        return (start, a.seq, pin)
    }

    private var timer: Timer?
    private var lastSeq: UInt64?
    private var shownPIN: String?
    private var busy = false
    private let socketPath: String
    private let onPIN: (String?) -> Void

    init(socketPath: String = VRControl.socketPath, onPIN: @escaping (String?) -> Void) {
        self.socketPath = socketPath
        self.onPIN = onPIN
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
    }

    deinit { timer?.invalidate() }

    private func tick() {
        guard VRHost.running != nil, !busy else { return }
        busy = true
        let path = socketPath
        Task.detached {
            let reply = VRControl.request("status", path: path).flatMap { try? JSONDecoder().decode(Reply.self, from: $0) }
            await MainActor.run {
                self.busy = false
                let a = Self.actions(reply, lastSeq: self.lastSeq)
                self.lastSeq = a.seq
                if let id = a.start { VRHost.launchInVR(id) }
                if a.pin != self.shownPIN {
                    self.shownPIN = a.pin
                    self.onPIN(a.pin)
                }
            }
        }
    }
}
