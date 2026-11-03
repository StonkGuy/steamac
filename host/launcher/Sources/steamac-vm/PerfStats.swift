import AppKit
import Foundation
import QuartzCore
import Metal
import os
import Synchronization

/// `--perf-stats` / STEAMAC_PERF_STATS=1 / Settings > General: every 5 s, log how guest frames
/// travel to the screen (switchable while the VM runs, `setEnabled`).
///
///   flush   guest flushes of scanout 0 (libkrun present_frame, GPU worker thread): interval
///           distribution and libkrun's per-flush copy into our frame (alloc_frame -> present_frame)
///   shown   new frames on screen: each drawable is shown at the first display refresh after its
///           command buffer completed (displaySyncEnabled), and never at the same refresh as the
///           previous one. The refresh grid comes from the view's CADisplayLink: macOS 15 reports
///           MTLDrawable.presentedTime 0 for nearly every drawable, so it cannot be used.
///           Interval distribution, refreshes per frame (`vsyncs 2:1194 3:4` = 1194 frames held for
///           2 refreshes, 4 for 3), flush -> screen latency, flushes replaced in the mailbox before
///           the main thread took them, main-thread upload and nextDrawable waits.
///
/// Intervals over 25 ms (more than 1.5 frames at 60 Hz) and 50 ms are the visible stutters.
///
/// STEAMAC_PERF_FRAMES=<path> (implies the stats): one CSV line per shown frame, appended to <path>
/// (header on a new file). Lines are buffered and written about once a second by a background
/// writer, so the render threads make no syscalls per frame. Columns:
///   t_ns       shown time (vsync-grid estimate) on the mach clock (DispatchTime.uptimeNanoseconds base)
///   flush_ms   interval from the previous guest flush to this frame's flush
///   shown_ms   interval from the previous shown frame
///   copy_ms    libkrun's copy of this frame (alloc_frame -> present_frame)
///   upload_ms  main-thread texture upload of this frame
///   vsyncs     refreshes this frame was held for (0 without a display link)
/// Off (env unset): no per-frame work; the stats path itself is off unless enabled.
final class PerfStats {
    static let instance = PerfStats()
    private static let enabled = Atomic<Bool>(false)
    /// The collector while stats are on (hot paths: one relaxed atomic load when off).
    static var shared: PerfStats? { enabled.load(ordering: .relaxed) ? instance : nil }

    static let period: TimeInterval = 5

    private var lock = os_unfair_lock()
    private var flushTimes: [CFTimeInterval] = []
    private var copyMs: [Double] = []
    private var allocAt: CFTimeInterval = 0
    private var lastFlushAt: CFTimeInterval = 0
    private var shownTimes: [CFTimeInterval] = []
    private var refreshesPerFrame: [Int: Int] = [:]
    private var latencyMs: [Double] = []
    private var uploadMs: [Double] = []
    private var drawableWaitMs: [Double] = []
    private var taken = 0
    private var lastFlush: CFTimeInterval = 0
    // Per-frame values of the latest flush (GPU thread writes, main thread snapshots in frameTaken).
    private var lastFlushDeltaMs: Double = 0
    private var lastCopyMs: Double = 0
    // Per-frame export (STEAMAC_PERF_FRAMES). framesOn and frameBuf: under `lock`; the rest: frameQueue.
    private var framesOn = false
    private var frameBuf: [UInt8] = []
    private var lastFrameShown: CFTimeInterval = 0
    private var frameFile: UnsafeMutablePointer<FILE>?
    private let frameQueue = DispatchQueue(label: "es.fxgam.steamac.perf-frames", qos: .utility)
    private var frameTimer: DispatchSourceTimer?
    private var lastShown: CFTimeInterval = 0
    private var vsyncBase: CFTimeInterval = 0
    private var vsyncPeriod: CFTimeInterval = 0
    private var timer: DispatchSourceTimer?
    private var displayLink: CADisplayLink?
    private let linkTarget = LinkTarget()

    private init() { linkTarget.stats = self }
    private weak var view: NSView?

    private func locked(_ body: () -> Void) {
        os_unfair_lock_lock(&lock)
        body()
        os_unfair_lock_unlock(&lock)
    }

    /// Per-frame values carried from the guest flush to the screen with the frame.
    struct FrameStamp {
        var flushedAt: CFTimeInterval = 0
        var flushMs: Double = 0
        var copyMs: Double = 0
        var uploadMs: Double = 0
    }

    /// Main thread: start / stop collecting and reporting.
    static func setEnabled(_ on: Bool) {
        guard enabled.load(ordering: .relaxed) != on else { return }
        instance.locked {
            instance.flushTimes.removeAll(); instance.copyMs.removeAll(); instance.shownTimes.removeAll()
            instance.refreshesPerFrame.removeAll(); instance.latencyMs.removeAll(); instance.uploadMs.removeAll()
            instance.drawableWaitMs.removeAll(); instance.taken = 0; instance.allocAt = 0
            instance.lastFlush = 0; instance.lastShown = 0
        }
        enabled.store(on, ordering: .relaxed)
        if on { instance.start() } else { instance.stop() }
    }

    private func start() {
        let t = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        t.schedule(deadline: .now() + PerfStats.period, repeating: PerfStats.period)
        t.setEventHandler { [weak self] in self?.report() }
        t.resume()
        timer = t
        if let view { attach(view: view) }
        startFrames()
        perfLog("perf: stats every \(Int(PerfStats.period)) s (intervals in ms; >25/>50 = intervals longer than that)")
    }

    private func stop() {
        timer?.cancel()
        timer = nil
        displayLink?.invalidate()
        displayLink = nil
        stopFrames()
        perfLog("perf: stats off")
    }

    /// Main thread: STEAMAC_PERF_FRAMES=<path> opens the per-frame CSV and starts its 1 s writer.
    private func startFrames() {
        guard let path = ProcessInfo.processInfo.environment["STEAMAC_PERF_FRAMES"], !path.isEmpty else { return }
        let opened: Bool = frameQueue.sync {
            guard let f = fopen(path, "a") else { return false }
            fseek(f, 0, SEEK_END)
            if ftell(f) == 0 { fputs("t_ns,flush_ms,shown_ms,copy_ms,upload_ms,vsyncs\n", f) }
            frameFile = f
            return true
        }
        guard opened else { perfLog("perf: STEAMAC_PERF_FRAMES: cannot open \(path)"); return }
        let t = DispatchSource.makeTimerSource(queue: frameQueue)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in self?.writeFrames() }
        t.resume()
        frameTimer = t
        locked { lastFrameShown = 0; framesOn = true }
        perfLog("perf: per-frame export to \(path)")
    }

    /// Main thread: stop accepting frames, write what is buffered, close the file.
    private func stopFrames() {
        guard let t = frameTimer else { return }
        locked { framesOn = false }
        frameTimer = nil
        t.cancel()
        frameQueue.sync {
            writeFrames()
            if let f = frameFile { fclose(f); frameFile = nil }
        }
    }

    /// frameQueue: drain the line buffer into the file.
    private func writeFrames() {
        var out: [UInt8] = []
        locked { swap(&out, &frameBuf) }
        guard !out.isEmpty, let f = frameFile else { return }
        out.withUnsafeBufferPointer { _ = fwrite($0.baseAddress, 1, $0.count, f) }
        fflush(f)
    }

    /// Main thread: follow the refreshes of the display showing `view` (while stats are on).
    func attach(view: NSView) {
        self.view = view
        displayLink?.invalidate()
        displayLink = nil
        guard PerfStats.shared != nil else { return }
        let link = view.displayLink(target: linkTarget, selector: #selector(LinkTarget.tick(_:)))
        link.add(to: .main, forMode: .common)
        displayLink = link
    }

    private final class LinkTarget: NSObject {
        weak var stats: PerfStats?
        @objc func tick(_ link: CADisplayLink) {
            guard let stats, link.duration > 0 else { return }
            let (t, d) = (link.timestamp, link.duration)
            stats.locked { stats.vsyncBase = t; stats.vsyncPeriod = d }
        }
    }

    // GPU worker thread (libkrun display callbacks).
    func frameAllocated() {
        let now = CACurrentMediaTime()
        locked { allocAt = now }
    }

    func framePresented() {
        let now = CACurrentMediaTime()
        locked {
            flushTimes.append(now)
            let copy = allocAt > 0 ? (now - allocAt) * 1000 : 0
            if allocAt > 0 { copyMs.append(copy) }
            lastCopyMs = copy
            allocAt = 0
            if lastFlushAt > 0 { lastFlushDeltaMs = (now - lastFlushAt) * 1000 }
            lastFlushAt = now
        }
    }

    // Main thread.
    /// The flush the presenter just took (the latest one), with its per-frame values.
    func frameTaken() -> FrameStamp {
        var s = FrameStamp()
        locked {
            taken += 1
            s.flushedAt = lastFlushAt; s.flushMs = lastFlushDeltaMs; s.copyMs = lastCopyMs
        }
        return s
    }

    func uploaded(ms: Double) { locked { uploadMs.append(ms) } }
    func waitedForDrawable(ms: Double) { locked { drawableWaitMs.append(ms) } }

    // Metal completion thread: the drawable's command buffer completed.
    func rendered(_ stamp: FrameStamp) {
        let now = CACurrentMediaTime()
        locked {
            var shown = now
            var refreshes = 0
            if vsyncPeriod > 0 {
                shown = vsyncBase + (((now - vsyncBase) / vsyncPeriod).rounded(.up)) * vsyncPeriod
                let prev = shownTimes.last ?? lastShown
                if prev > 0 {
                    // One new drawable per refresh at most: a second one waits for the next.
                    if shown < prev + vsyncPeriod / 2 { shown = prev + vsyncPeriod }
                    refreshes = Int(((shown - prev) / vsyncPeriod).rounded())
                    refreshesPerFrame[refreshes, default: 0] += 1
                }
            }
            if framesOn { appendFrame(shown: shown, refreshes: refreshes, stamp: stamp) }
            shownTimes.append(shown)
            if stamp.flushedAt > 0 { latencyMs.append((shown - stamp.flushedAt) * 1000) }
        }
    }

    /// Under `lock` (Metal thread): one CSV line into the buffer; the writer drains it.
    private func appendFrame(shown: CFTimeInterval, refreshes: Int, stamp: FrameStamp) {
        let shownMs = lastFrameShown > 0 ? (shown - lastFrameShown) * 1000 : 0
        lastFrameShown = shown
        let line = String(format: "%llu,%.3f,%.3f,%.3f,%.3f,%ld\n",
                          UInt64(max(0, shown) * 1e9), stamp.flushMs, shownMs, stamp.copyMs, stamp.uploadMs, refreshes)
        frameBuf.append(contentsOf: line.utf8)
    }

    private func report() {
        var flushes: [CFTimeInterval] = [], shown: [CFTimeInterval] = []
        var copy: [Double] = [], lat: [Double] = [], up: [Double] = [], wait: [Double] = []
        var hist: [Int: Int] = [:]
        var took = 0, prevFlush: CFTimeInterval = 0, prevShown: CFTimeInterval = 0, refresh: CFTimeInterval = 0
        locked {
            swap(&flushes, &flushTimes); swap(&shown, &shownTimes)
            swap(&copy, &copyMs); swap(&lat, &latencyMs); swap(&up, &uploadMs); swap(&wait, &drawableWaitMs)
            swap(&hist, &refreshesPerFrame)
            took = taken; taken = 0
            prevFlush = lastFlush; prevShown = lastShown
            if let l = flushes.last { lastFlush = l }
            if let l = shown.last { lastShown = l }
            refresh = vsyncPeriod
        }
        // Intervals continue across reports (the first one starts at the previous report's last frame).
        func intervals(_ ts: [CFTimeInterval], from prev: CFTimeInterval) -> [Double] {
            var out: [Double] = []
            var p = prev
            for t in ts {
                if p > 0 { out.append((t - p) * 1000) }
                p = t
            }
            return out
        }
        let fi = intervals(flushes, from: prevFlush), si = intervals(shown, from: prevShown)
        let vsyncs = hist.keys.sorted().map { "\($0):\(hist[$0]!)" }.joined(separator: " ")
        var line = "perf: flush n=\(flushes.count) \(PerfStats.dist(fi))"
        line += " copy avg/max=\(PerfStats.avgMax(copy))"
        line += " | shown n=\(shown.count) \(PerfStats.dist(si))"
        line += " vsyncs(\(refresh > 0 ? String(format: "%.0f Hz", 1 / refresh) : "?")) \(vsyncs.isEmpty ? "-" : vsyncs)"
        line += " lat p50/p99=\(PerfStats.f(PerfStats.pct(lat, 50)))/\(PerfStats.f(PerfStats.pct(lat, 99)))"
        line += " replaced=\(max(0, flushes.count - took))"
        line += " upload avg/max=\(PerfStats.avgMax(up)) drawable-wait max=\(PerfStats.f(wait.max() ?? 0))"
        perfLog(line)
    }

    private static func f(_ v: Double) -> String { String(format: "%.1f", v) }

    private static func pct(_ v: [Double], _ p: Double) -> Double {
        guard !v.isEmpty else { return 0 }
        let s = v.sorted()
        return s[min(s.count - 1, Int((Double(s.count - 1) * p / 100).rounded()))]
    }

    private static func dist(_ v: [Double]) -> String {
        // +0.5 ms: shown intervals are whole refreshes (3 x 8.33 ms must count as > 25).
        let over25 = v.filter { $0 > 24.5 }.count, over50 = v.filter { $0 > 49.5 }.count
        return "p50/p95/p99/max=\(f(pct(v, 50)))/\(f(pct(v, 95)))/\(f(pct(v, 99)))/\(f(v.max() ?? 0)) >25=\(over25) >50=\(over50)"
    }

    private static func avgMax(_ v: [Double]) -> String {
        guard !v.isEmpty else { return "0.0/0.0" }
        return "\(f(v.reduce(0, +) / Double(v.count)))/\(f(v.max()!))"
    }
}

/// Perf lines carry an ISO8601 wall-clock prefix (local offset) ahead of "[steamac-vm] perf:", so
/// they align with guest samples. The "] perf: " marker stays intact for the support report filter.
func perfLog(_ message: String) {
    let stamp = ISO8601DateFormatter.string(from: Date(), timeZone: .current,
                                            formatOptions: [.withInternetDateTime, .withFractionalSeconds])
    LogOutput.write("\(stamp) [steamac-vm] \(message)\n")
    CrashReporting.logged(message)
}

/// Independent of the performance-statistics setting and of the main queue.
/// Logical Vulkan/shm/Metal totals overlap; none may be added to phys_footprint.
/// Keeping the categories together distinguishes live application allocations,
/// deferred shm reclamation and driver/private growth in the next hang report.
final class VMMemoryDiagnostics: @unchecked Sendable {
    static let shared = VMMemoryDiagnostics()
    private let lock = NSLock()
    private var timer: DispatchSourceTimer?
    private var samples: [String] = []
    private let device = MTLCreateSystemDefaultDevice()
    private typealias VulkanStats = @convention(c) (UnsafeMutablePointer<UInt64>) -> Void
    private let vulkanStats: VulkanStats? = {
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "virgl_steamac_memory_stats") else { return nil }
        return unsafeBitCast(symbol, to: VulkanStats.self)
    }()

    private init() { samples.reserveCapacity(12) }

    func start() {
        lock.lock()
        defer { lock.unlock() }
        guard timer == nil else { return }
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        timer.schedule(deadline: .now(), repeating: 10)
        timer.setEventHandler { [weak self] in self?.sample() }
        self.timer = timer
        timer.resume()
    }

    func history() -> String {
        lock.lock(); defer { lock.unlock() }
        return "VM memory snapshots (MiB; overlapping logical categories, not additive):\n"
            + samples.joined(separator: "\n")
    }

    private func sample() {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        var line = "t=\(Int(ProcessInfo.processInfo.systemUptime))"
        if result == KERN_SUCCESS {
            line += " footprint=\(info.phys_footprint >> 20) resident=\(info.resident_size >> 20)"
                + " internal=\(info.internal >> 20) compressed=\(info.compressed >> 20)"
                + " external=\(info.external >> 20) reusable=\(info.reusable >> 20)"
        } else {
            line += " task-info-error=\(result)"
        }
        if let device {
            line += " metal-allocated=\(device.currentAllocatedSize >> 20)"
                + " metal-recommended=\(device.recommendedMaxWorkingSetSize >> 20)"
        }
        if let vulkanStats {
            var values = (UInt64(0), UInt64(0), UInt64(0), UInt64(0))
            withUnsafeMutableBytes(of: &values) { vulkanStats($0.bindMemory(to: UInt64.self).baseAddress!) }
            line += " vkr-live=\(values.0 >> 20) vkr-count=\(values.1)"
                + " shm-mapped=\(values.2 >> 20) shm-deferred=\(values.3 >> 20)"
        } else {
            line += " vkr-stats=unavailable"
        }
        lock.lock()
        if samples.count == 12 { samples.removeFirst() }
        samples.append(line)
        lock.unlock()
        log("memory: " + line + " (MiB; overlapping categories)")
    }
}
