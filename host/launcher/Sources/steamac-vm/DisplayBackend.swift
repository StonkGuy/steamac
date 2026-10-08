import CKrun
import Foundation
import os

/// One CPU frame buffer handed to libkrun's GPU thread (filled by rutabaga transfer_read).
/// Immutable geometry; the consumer keeps a strong reference while reading so a
/// reconfigure on the GPU thread never frees memory that is still being uploaded.
final class FrameBuffer {
    let ptr: UnsafeMutableRawPointer
    let size: Int
    let width: Int
    let height: Int
    let stride: Int
    let format: UInt32

    init(width: Int, height: Int, format: UInt32) {
        self.width = width
        self.height = height
        self.stride = width * 4
        self.format = format
        self.size = stride * height
        ptr = UnsafeMutableRawPointer.allocate(byteCount: max(size, 1), alignment: 16384)
        ptr.initializeMemory(as: UInt8.self, repeating: 0, count: max(size, 1))
    }

    deinit { ptr.deallocate() }
}

/// Pixel formats libkrun can hand us (virtio-gpu format numbers; names are byte order in memory).
enum ScanoutFormat {
    /// Byte offsets of R, G, B within a 4-byte pixel, or nil for unknown formats.
    static func rgbOffsets(_ format: UInt32) -> (r: Int, g: Int, b: Int)? {
        switch Int32(format) {
        case KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM, KRUN_DISPLAY_FORMAT_B8G8R8X8_UNORM: return (2, 1, 0)
        case KRUN_DISPLAY_FORMAT_A8R8G8B8_UNORM, KRUN_DISPLAY_FORMAT_X8R8G8B8_UNORM: return (1, 2, 3)
        case KRUN_DISPLAY_FORMAT_R8G8B8A8_UNORM, KRUN_DISPLAY_FORMAT_R8G8B8X8_UNORM: return (0, 1, 2)
        case KRUN_DISPLAY_FORMAT_X8B8G8R8_UNORM, KRUN_DISPLAY_FORMAT_A8B8G8R8_UNORM: return (3, 2, 1)
        default: return nil
        }
    }

    static let all: [UInt32] = [
        KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM, KRUN_DISPLAY_FORMAT_B8G8R8X8_UNORM,
        KRUN_DISPLAY_FORMAT_A8R8G8B8_UNORM, KRUN_DISPLAY_FORMAT_X8R8G8B8_UNORM,
        KRUN_DISPLAY_FORMAT_R8G8B8A8_UNORM, KRUN_DISPLAY_FORMAT_X8B8G8R8_UNORM,
        KRUN_DISPLAY_FORMAT_A8B8G8R8_UNORM, KRUN_DISPLAY_FORMAT_R8G8B8X8_UNORM,
    ].map { UInt32($0) }

    static func name(_ format: UInt32) -> String {
        switch Int32(format) {
        case KRUN_DISPLAY_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8"
        case KRUN_DISPLAY_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8"
        case KRUN_DISPLAY_FORMAT_A8R8G8B8_UNORM: return "A8R8G8B8"
        case KRUN_DISPLAY_FORMAT_X8R8G8B8_UNORM: return "X8R8G8B8"
        case KRUN_DISPLAY_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8"
        case KRUN_DISPLAY_FORMAT_X8B8G8R8_UNORM: return "X8B8G8R8"
        case KRUN_DISPLAY_FORMAT_A8B8G8R8_UNORM: return "A8B8G8R8"
        case KRUN_DISPLAY_FORMAT_R8G8B8X8_UNORM: return "R8G8B8X8"
        default: return "format\(format)"
        }
    }
}

struct DamageRect {
    var x0: Int, y0: Int, x1: Int, y1: Int   // half-open [x0,x1) x [y0,y1)

    func union(_ o: DamageRect) -> DamageRect {
        DamageRect(x0: min(x0, o.x0), y0: min(y0, o.y0), x1: max(x1, o.x1), y1: max(y1, o.y1))
    }

    func clamped(width: Int, height: Int) -> DamageRect? {
        let r = DamageRect(x0: max(0, x0), y0: max(0, y0), x1: min(width, x1), y1: min(height, y1))
        return (r.x1 > r.x0 && r.y1 > r.y0) ? r : nil
    }
}

/// A frame taken by the consumer (main thread). `damage == nil` means "upload everything".
struct TakenFrame {
    let buffer: FrameBuffer
    let damage: DamageRect?
    let generation: Int
}

/// Per-scanout frame mailbox. Three buffers: the GPU thread never writes into the frame
/// that is ready (= last presented) or the one the consumer is uploading.
final class Scanout {
    let id: UInt32
    private var lock = os_unfair_lock()
    private var buffers: [FrameBuffer] = []
    private var writing: Int?
    private var ready: Int?
    private var displaying: Int?
    private var lastPresented: Int?
    private var damage: DamageRect?
    private var fullDamage = true
    private(set) var generation = 0
    private(set) var enabled = false
    private(set) var presented: UInt64 = 0

    static let bufferCount = 3

    init(id: UInt32) { self.id = id }

    private func locked<T>(_ body: () -> T) -> T {
        os_unfair_lock_lock(&lock)
        defer { os_unfair_lock_unlock(&lock) }
        return body()
    }

    /// Returns true if geometry/format changed (new generation).
    func configure(width: Int, height: Int, format: UInt32) -> Bool {
        locked {
            enabled = true
            if let b = buffers.first, b.width == width, b.height == height, b.format == format {
                return false
            }
            buffers = (0..<Scanout.bufferCount).map { _ in FrameBuffer(width: width, height: height, format: format) }
            writing = nil; ready = nil; displaying = nil; lastPresented = nil
            damage = nil; fullDamage = true
            generation += 1
            return true
        }
    }

    func disable() {
        locked {
            enabled = false
            buffers = []
            writing = nil; ready = nil; displaying = nil; lastPresented = nil
            damage = nil; fullDamage = true
            generation += 1
        }
    }

    var geometry: (width: Int, height: Int, format: UInt32)? {
        locked { buffers.first.map { ($0.width, $0.height, $0.format) } }
    }

    func alloc() -> (Int32, UnsafeMutableRawPointer, Int)? {
        locked {
            guard !buffers.isEmpty else { return nil }
            for i in 0..<buffers.count where i != ready && i != displaying && i != lastPresented {
                writing = i
                return (Int32(i), buffers[i].ptr, buffers[i].size)
            }
            return nil
        }
    }

    /// Returns false if frameId was not the outstanding allocation.
    func present(frameId: Int32, rect: DamageRect?) -> Bool {
        locked {
            guard let w = writing, w == Int(frameId) else { return false }
            writing = nil
            ready = w
            lastPresented = w
            presented &+= 1
            if let rect, !fullDamage {
                damage = damage.map { $0.union(rect) } ?? rect
            } else {
                fullDamage = true
            }
            return true
        }
    }

    func take() -> TakenFrame? {
        locked {
            guard let r = ready else { return nil }
            ready = nil
            displaying = r
            let d: DamageRect? = fullDamage ? nil : damage
            damage = nil
            fullDamage = false
            return TakenFrame(buffer: buffers[r], damage: d, generation: generation)
        }
    }

    func release(_ frame: TakenFrame) {
        locked {
            if frame.generation == generation { displaying = nil }
        }
    }

    /// Copy of the most recently presented frame (safe: the GPU thread never writes into it).
    func snapshot() -> (data: Data, width: Int, height: Int, format: UInt32)? {
        locked {
            guard let i = lastPresented else { return nil }
            let b = buffers[i]
            return (Data(bytes: b.ptr, count: b.size), b.width, b.height, b.format)
        }
    }

    /// What the window can show, for NoPictureGuard (main thread, a few times a second).
    struct Probe {
        var enabled: Bool
        /// A frame was presented since the last scanout set / resize.
        var hasPicture: Bool
        var presented: UInt64
        /// Fraction of sample points darker than `darkLuma` in the latest frame; nil = not sampled
        /// (no frame, or none newer than the caller has seen).
        var dark: Double?
    }

    static let sampleColumns = 64
    static let sampleRows = 40
    /// Rec. 709 luma below this (0...255) counts as black.
    static let darkLuma = 8

    /// Scanout state, plus the darkness of the latest frame if one newer than `seen` (a
    /// `presented` value) arrived. The GPU thread never writes into the last presented frame, so
    /// it is safe to sample it after the lock is dropped (the 2560 scattered reads must not hold
    /// the lock the GPU present worker takes).
    func probe(sampleIfNewerThan seen: UInt64) -> Probe {
        let (p, sample) = locked { () -> (Probe, FrameBuffer?) in
            let p = Probe(enabled: enabled, hasPicture: lastPresented != nil, presented: presented, dark: nil)
            return (p, lastPresented.flatMap { presented != seen ? buffers[$0] : nil })
        }
        guard let frame = sample else { return p }
        var result = p
        result.dark = Scanout.darkFraction(frame)
        return result
    }

    /// Sparse black test: luma at a `sampleColumns`×`sampleRows` grid of pixel centres (2560 reads,
    /// a few microseconds); the fraction of them below `darkLuma`.
    static func darkFraction(_ b: FrameBuffer) -> Double {
        guard let (r, g, bl) = ScanoutFormat.rgbOffsets(b.format), b.width > 0, b.height > 0 else { return 0 }
        let px = UnsafeRawPointer(b.ptr).assumingMemoryBound(to: UInt8.self)
        let limit = darkLuma * 256
        var dark = 0
        for j in 0..<sampleRows {
            let row = px + ((2 * j + 1) * b.height / (2 * sampleRows)) * b.stride
            for i in 0..<sampleColumns {
                let p = row + ((2 * i + 1) * b.width / (2 * sampleColumns)) * 4
                // Rec. 709 weights in 1/256 units.
                if 54 * Int(p[r]) + 183 * Int(p[g]) + 19 * Int(p[bl]) < limit { dark += 1 }
            }
        }
        return Double(dark) / Double(sampleColumns * sampleRows)
    }
}

/// Receives notifications from libkrun's GPU thread. Implementations must be cheap and non-blocking.
protocol DisplaySink: AnyObject {
    func scanoutConfigured(_ scanout: Scanout, changed: Bool)
    func scanoutDisabled(_ scanout: Scanout)
    func framePresented(_ scanout: Scanout)
}

/// libkrun display backend (KRUN_DISPLAY_FEATURE_BASIC_FRAMEBUFFER), all callbacks arrive on
/// libkrun's GPU worker thread.
final class DisplayBackend {
    let scanouts: [Scanout] = (0..<UInt32(KRUN_MAX_DISPLAYS)).map { Scanout(id: $0) }
    weak var sink: DisplaySink?
    var verbose = false

    init() {}

    /// The C struct handed to krun_set_display_backend. `self` must outlive the VM (it does:
    /// the backend is kept in a global for the process lifetime).
    func makeCBackend() -> krun_display_backend {
        var b = krun_display_backend()
        b.features = UInt64(KRUN_DISPLAY_FEATURE_BASIC_FRAMEBUFFER)
        b.create_userdata = Unmanaged.passUnretained(self).toOpaque()
        b.create = { instance, userdata, _ in
            instance?.pointee = UnsafeMutableRawPointer(mutating: userdata)
            return 0
        }
        b.vtable.basic_framebuffer.destroy = { _ in 0 }
        b.vtable.basic_framebuffer.configure_scanout = { inst, id, dw, dh, w, h, fmt in
            DisplayBackend.from(inst).configureScanout(id: id, displayWidth: dw, displayHeight: dh, width: w, height: h, format: fmt)
        }
        b.vtable.basic_framebuffer.disable_scanout = { inst, id in
            DisplayBackend.from(inst).disableScanout(id: id)
        }
        b.vtable.basic_framebuffer.alloc_frame = { inst, id, buffer, size in
            DisplayBackend.from(inst).allocFrame(id: id, buffer: buffer, size: size)
        }
        b.vtable.basic_framebuffer.present_frame = { inst, id, frame, rect in
            DisplayBackend.from(inst).presentFrame(id: id, frameId: frame, rect: rect)
        }
        return b
    }

    static func from(_ inst: UnsafeMutableRawPointer?) -> DisplayBackend {
        Unmanaged<DisplayBackend>.fromOpaque(inst!).takeUnretainedValue()
    }

    func configureScanout(id: UInt32, displayWidth: UInt32, displayHeight: UInt32,
                          width: UInt32, height: UInt32, format: UInt32) -> Int32 {
        guard Int(id) < scanouts.count else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        guard ScanoutFormat.rgbOffsets(format) != nil else {
            log("display: scanout \(id): unsupported format \(format)")
            return KRUN_DISPLAY_ERR_INVALID_PARAM
        }
        guard width > 0, height > 0, width <= 16384, height <= 16384 else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
        let s = scanouts[Int(id)]
        let changed = s.configure(width: Int(width), height: Int(height), format: format)
        if changed {
            log("display: scanout \(id) \(width)x\(height) \(ScanoutFormat.name(format)) (display \(displayWidth)x\(displayHeight))")
        }
        sink?.scanoutConfigured(s, changed: changed)
        return 0
    }

    func disableScanout(id: UInt32) -> Int32 {
        guard Int(id) < scanouts.count else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        let s = scanouts[Int(id)]
        s.disable()
        log("display: scanout \(id) disabled")
        sink?.scanoutDisabled(s)
        return 0
    }

    func allocFrame(id: UInt32, buffer: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?,
                    size: UnsafeMutablePointer<Int>?) -> Int32 {
        guard Int(id) < scanouts.count else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        guard let (frameId, ptr, len) = scanouts[Int(id)].alloc() else {
            return scanouts[Int(id)].geometry == nil ? KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID : KRUN_DISPLAY_ERR_OUT_OF_BUFFERS
        }
        buffer?.pointee = ptr.assumingMemoryBound(to: UInt8.self)
        size?.pointee = len
        if id == 0 { PerfStats.shared?.frameAllocated() }
        return frameId
    }

    func presentFrame(id: UInt32, frameId: UInt32, rect: UnsafePointer<krun_rect>?) -> Int32 {
        guard Int(id) < scanouts.count else { return KRUN_DISPLAY_ERR_INVALID_SCANOUT_ID }
        let s = scanouts[Int(id)]
        var damage: DamageRect?
        if let r = rect?.pointee {
            damage = DamageRect(x0: Int(r.x), y0: Int(r.y), x1: Int(r.x) + Int(r.width), y1: Int(r.y) + Int(r.height))
        }
        guard s.present(frameId: Int32(bitPattern: frameId), rect: damage) else { return KRUN_DISPLAY_ERR_INVALID_PARAM }
        if id == 0 { PerfStats.shared?.framePresented() }
        sink?.framePresented(s)
        return 0
    }

    /// Write scanout `id`'s last presented frame to `path` as PNG.
    func dumpPNG(scanout id: Int = 0, to path: String) throws {
        guard let snap = scanouts[id].snapshot() else { throw OptionError("no frame presented on scanout \(id) yet") }
        try PNG.write(bgrxLike: snap.data, width: snap.width, height: snap.height, format: snap.format, to: path)
    }
}
