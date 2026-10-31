import Foundation
import IOKit.hid

/// The Mac's DualSense controllers as raw HID devices (IOHIDManager), for passing one through to
/// the guest as a uhid device (GamepadBridge, `hid-create` on fx.pad): the guest's hid-playstation
/// driver and Steam's own DualSense support then get the controller's real reports — touchpad,
/// motion sensors, mute button — and drive its lightbar, player and mute LEDs, rumble and
/// adaptive triggers with their own output reports.
///
/// GameController keeps the device too (it still picks the controller and wakes a sleeping
/// guest): both read the same input reports; the IOHIDManager is opened without seizing it.
/// Reports keep the HID convention on both sides: a numbered report starts with its ID.
/// Callbacks run on the main run loop; Get/SetReport block, so they run on a serial queue and
/// complete on the main queue.
final class HIDPassthrough {
    /// DualSense, DualSense Edge (the products hid-playstation binds to as a DualSense).
    static let supported: [(vendor: Int, product: Int)] = [(0x054c, 0x0ce6), (0x054c, 0x0df2)]

    /// What the guest's uhid device is created with (`hid-create`).
    struct Identity: Equatable {
        /// Linux BUS_USB / BUS_BLUETOOTH: hid-playstation's report layouts depend on it.
        let bus: UInt16
        let vendor: UInt16
        let product: UInt16
        let version: UInt16
        let country: UInt32
        let descriptor: [UInt8]
        let name: String

        var createLine: String {
            String(format: "hid-create %04x %04x %04x %04x %x ", bus, vendor, product, version, country)
                + HIDPassthrough.hex(descriptor) + " " + name
        }
    }

    final class Device {
        let ref: IOHIDDevice
        let identity: Identity
        let maxInput: Int
        let maxFeature: Int
        /// Where IOKit copies the device's input reports, for as long as the device lives.
        /// IOHIDLib's device plugin keeps the buffer of the last input report callback
        /// registered and copies every report into it while the device is open, also after the
        /// callback is unregistered (a nil callback only drops it from IOHIDDevice's callback
        /// set): a buffer freed on deactivation took the next reports of a still connected
        /// controller (STEAMAC-35: a USB DualSense's 64-byte reports overwrote a freed 64-byte
        /// block that libobjc had reused for a GameController class's method cache, abort
        /// "Method cache corrupted").
        fileprivate let inputBuffer: UnsafeMutablePointer<UInt8>

        init(ref: IOHIDDevice, identity: Identity, maxInput: Int, maxFeature: Int) {
            self.ref = ref
            self.identity = identity
            self.maxInput = maxInput
            self.maxFeature = maxFeature
            inputBuffer = .allocate(capacity: maxInput)
        }

        /// Only on the main thread (HIDPassthrough keeps devices there): its reports are
        /// delivered on the main run loop, which this unschedules before the buffer goes.
        deinit {
            IOHIDDeviceUnscheduleFromRunLoop(ref, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
            inputBuffer.deallocate()
        }
    }

    private var manager: IOHIDManager?
    /// Connected supported devices in connection order.
    private(set) var devices: [Device] = []
    /// The device whose input reports go to `onInput`.
    private(set) var active: Device?
    /// Devices came or went.
    var onChange: (() -> Void)?
    /// An input report of the active device (main thread).
    var onInput: ((UnsafeBufferPointer<UInt8>) -> Void)?
    private let io = DispatchQueue(label: "es.fxgam.steamac.hid-passthrough")

    /// Start watching for supported devices; false if the IOHIDManager cannot be opened.
    @discardableResult
    func start() -> Bool {
        let m = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
        let matching = HIDPassthrough.supported.map {
            [kIOHIDVendorIDKey: $0.vendor, kIOHIDProductIDKey: $0.product] as CFDictionary
        }
        IOHIDManagerSetDeviceMatchingMultiple(m, matching as CFArray)
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        IOHIDManagerRegisterDeviceMatchingCallback(m, { ctx, _, _, device in
            guard let ctx else { return }
            Unmanaged<HIDPassthrough>.fromOpaque(ctx).takeUnretainedValue().added(device)
        }, ctx)
        IOHIDManagerRegisterDeviceRemovalCallback(m, { ctx, _, _, device in
            guard let ctx else { return }
            Unmanaged<HIDPassthrough>.fromOpaque(ctx).takeUnretainedValue().removed(device)
        }, ctx)
        IOHIDManagerScheduleWithRunLoop(m, CFRunLoopGetMain(), CFRunLoopMode.defaultMode.rawValue)
        let r = IOHIDManagerOpen(m, IOOptionBits(kIOHIDOptionsTypeNone))
        manager = m
        guard r == kIOReturnSuccess else {
            log("hid passthrough: cannot open the HID manager: \(HIDPassthrough.describe(r))")
            return false
        }
        return true
    }

    private func added(_ ref: IOHIDDevice) {
        guard !devices.contains(where: { $0.ref == ref }) else { return }
        guard let d = HIDPassthrough.device(ref) else { return }
        devices.append(d)
        log("hid passthrough: found \(d.identity.name) (\(d.identity.bus == HIDPassthrough.busBluetooth ? "Bluetooth" : "USB"), "
            + "\(d.identity.descriptor.count)-byte descriptor, input reports up to \(d.maxInput) bytes)")
        onChange?()
    }

    private func removed(_ ref: IOHIDDevice) {
        guard let i = devices.firstIndex(where: { $0.ref == ref }) else { return }
        let d = devices.remove(at: i)
        if active === d { deactivate() }
        log("hid passthrough: \(d.identity.name) disconnected")
        onChange?()
    }

    static let busUSB: UInt16 = 0x03
    static let busBluetooth: UInt16 = 0x05

    private static func device(_ ref: IOHIDDevice) -> Device? {
        func int(_ key: String) -> Int? { (IOHIDDeviceGetProperty(ref, key as CFString) as? NSNumber)?.intValue }
        func string(_ key: String) -> String? { IOHIDDeviceGetProperty(ref, key as CFString) as? String }
        let transport = string(kIOHIDTransportKey) ?? "?"
        let bus: UInt16
        switch transport {
        case kIOHIDTransportUSBValue: bus = busUSB
        case kIOHIDTransportBluetoothValue: bus = busBluetooth
        default:
            log("hid passthrough: skipping a controller on transport \(transport)")
            return nil
        }
        guard let vendor = int(kIOHIDVendorIDKey), let product = int(kIOHIDProductIDKey),
              let descriptor = IOHIDDeviceGetProperty(ref, kIOHIDReportDescriptorKey as CFString) as? Data,
              !descriptor.isEmpty, descriptor.count <= 4096 else {
            log("hid passthrough: skipping a controller without a usable report descriptor")
            return nil
        }
        // The names Linux gives the controller (hid-playstation does not depend on them).
        let productName = string(kIOHIDProductKey) ?? "DualSense Wireless Controller"
        let name = bus == busUSB ? "\(string(kIOHIDManufacturerKey) ?? "Sony Interactive Entertainment") \(productName)" : productName
        let identity = Identity(bus: bus, vendor: UInt16(truncatingIfNeeded: vendor), product: UInt16(truncatingIfNeeded: product),
                                version: UInt16(truncatingIfNeeded: int(kIOHIDVersionNumberKey) ?? 0),
                                country: UInt32(truncatingIfNeeded: int(kIOHIDCountryCodeKey) ?? 0),
                                descriptor: [UInt8](descriptor), name: name)
        return Device(ref: ref, identity: identity,
                      maxInput: max(64, int(kIOHIDMaxInputReportSizeKey) ?? 0),
                      maxFeature: max(64, int(kIOHIDMaxFeatureReportSizeKey) ?? 0))
    }

    /// Input reports of `d` go to `onInput` from now on (none of any other device).
    func activate(_ d: Device) {
        guard active !== d else { return }
        deactivate()
        active = d
        let ctx = Unmanaged.passUnretained(self).toOpaque()
        IOHIDDeviceRegisterInputReportCallback(d.ref, d.inputBuffer, d.maxInput, { ctx, result, _, _, _, report, length in
            guard let ctx, result == kIOReturnSuccess, length > 0 else { return }
            let me = Unmanaged<HIDPassthrough>.fromOpaque(ctx).takeUnretainedValue()
            me.onInput?(UnsafeBufferPointer(start: report, count: length))
        }, ctx)
    }

    /// A nil callback unregisters ours (callbacks run on this, the main thread). IOKit keeps
    /// copying the device's reports into its buffer, which lives as long as the Device.
    func deactivate() {
        guard let d = active else { return }
        IOHIDDeviceRegisterInputReportCallback(d.ref, d.inputBuffer, d.maxInput, nil, nil)
        active = nil
    }

    enum ReportType: String {
        case feature, output, input

        var io: IOHIDReportType {
            switch self {
            case .feature: return kIOHIDReportTypeFeature
            case .output: return kIOHIDReportTypeOutput
            case .input: return kIOHIDReportTypeInput
            }
        }
    }

    /// GET_REPORT of report `id`: the report (ID first) or an error, on the main queue.
    func getReport(_ d: Device, type: ReportType, id: UInt8, done: @escaping (Result<[UInt8], IOReturnError>) -> Void) {
        // The IOHIDDevice, not the Device: a Device is released on the main thread only (deinit).
        let ref = d.ref, size = type == .input ? d.maxInput : d.maxFeature
        io.async {
            var buf = [UInt8](repeating: 0, count: size)
            buf[0] = id
            var len = CFIndex(size)
            let r = buf.withUnsafeMutableBufferPointer {
                IOHIDDeviceGetReport(ref, type.io, CFIndex(id), $0.baseAddress!, &len)
            }
            let result: Result<[UInt8], IOReturnError> =
                r == kIOReturnSuccess ? .success(Array(buf.prefix(len))) : .failure(IOReturnError(code: r))
            DispatchQueue.main.async { done(result) }
        }
    }

    /// SET_REPORT / an output report (ID first, as the guest sends it), done on the main queue.
    func setReport(_ d: Device, type: ReportType, data: [UInt8], done: ((IOReturn) -> Void)? = nil) {
        guard !data.isEmpty else { done?(kIOReturnBadArgument); return }
        let ref = d.ref
        io.async {
            let r = data.withUnsafeBufferPointer {
                IOHIDDeviceSetReport(ref, type.io, CFIndex(data[0]), $0.baseAddress!, data.count)
            }
            if r != kIOReturnSuccess { log("hid passthrough: \(type.rawValue) report \(data[0]): \(HIDPassthrough.describe(r))") }
            if let done { DispatchQueue.main.async { done(r) } }
        }
    }

    struct IOReturnError: Error {
        let code: IOReturn
    }

    static func describe(_ r: IOReturn) -> String {
        if r == kIOReturnNotPermitted {
            return "not permitted (System Settings > Privacy & Security > Input Monitoring)"
        }
        return String(format: "IOReturn 0x%08x", UInt32(bitPattern: r))
    }

    static func hex(_ bytes: UnsafeBufferPointer<UInt8>) -> String {
        let digits = Array("0123456789abcdef".utf8)
        var out = [UInt8](repeating: 0, count: bytes.count * 2)
        for (i, b) in bytes.enumerated() {
            out[2 * i] = digits[Int(b >> 4)]
            out[2 * i + 1] = digits[Int(b & 15)]
        }
        return String(decoding: out, as: UTF8.self)
    }

    static func hex(_ bytes: [UInt8]) -> String {
        bytes.withUnsafeBufferPointer { hex($0) }
    }

    /// Lowercase or uppercase hex to bytes; nil for an odd length or a non-hex digit.
    static func unhex(_ s: Substring) -> [UInt8]? {
        let u = Array(s.utf8)
        guard u.count % 2 == 0 else { return nil }
        func digit(_ c: UInt8) -> UInt8? {
            switch c {
            case 0x30...0x39: return c - 0x30
            case 0x61...0x66: return c - 0x61 + 10
            case 0x41...0x46: return c - 0x41 + 10
            default: return nil
            }
        }
        var out = [UInt8]()
        out.reserveCapacity(u.count / 2)
        var i = 0
        while i < u.count {
            guard let hi = digit(u[i]), let lo = digit(u[i + 1]) else { return nil }
            out.append(hi << 4 | lo)
            i += 2
        }
        return out
    }
}
