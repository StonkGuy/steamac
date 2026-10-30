import Foundation

/// --selftest-graphics-path: the graphics path this launch resolved (Settings > Advanced "Allow DXMT", or
/// --wmtr-lib / --no-wmtr / --graphics-path) reaches the VM's kernel command line and the environment libkrun
/// reads, and the other path's environment is cleared. Run through `open` with only `defaults write ... wmtr`
/// changed, this shows that a plain double-click start enables the chosen path. The Mullion path (experimental)
/// takes Mullion's own executor, never DXMT's or a chosen DXMT library: from --graphics-path mullion and from the
/// `mullion` setting with Allow DXMT on; a bundled Mullion executor must load.
enum GraphicsPathSelfTest {
    static func run(_ resolved: Options) -> Never {
        var failures = 0
        func check(_ ok: Bool, _ what: String) { if !ok { failures += 1; log("selftest-graphics-path: FAIL \(what)") } }
        log("selftest-graphics-path: resolved \(resolved.graphicsPath.rawValue), cmdline \(resolved.cmdline)")
        check(resolved.cmdline.split(separator: " ").contains("steamac.gfx=\(resolved.graphicsPath.rawValue)"),
              "kernel command line lacks steamac.gfx=\(resolved.graphicsPath.rawValue)")
        typealias Wmtr = LauncherSettings.Wmtr
        for path in [resolved.graphicsPath] + LauncherSettings.GraphicsPath.allCases.filter({ $0 != resolved.graphicsPath }) {
            var o = resolved
            o.mullion = path == .mullion
            o.wmtrLibrary = switch path {
            case .dxmt: resolved.graphicsPath == .dxmt ? resolved.wmtrLibrary : Wmtr.path(for: "")
            case .mullion: resolved.graphicsPath == .mullion ? resolved.wmtrLibrary : Wmtr.mullionPath
            case .vulkan: nil
            }
            check(o.graphicsPath == path, "\(path.name): options resolve to \(o.graphicsPath.name)")
            // left over from a shell or an earlier start: must not survive the Vulkan path
            setenv("STEAMAC_WMTR", "1", 1); setenv("STEAMAC_WMTR_LIB", "/nonexistent", 1)
            VM.applyGraphicsEnv(o)
            let wmtr = ProcessInfo.processInfo.environment["STEAMAC_WMTR"]
            let lib = ProcessInfo.processInfo.environment["STEAMAC_WMTR_LIB"]
            if path != .vulkan, let want = o.wmtrLibrary, let reason = Wmtr.unavailableReason(want) {
                // Options.resolve drops an executor that does not load, so a start never gets here with it
                log("selftest-graphics-path: \(path.name) executor not checked here (\(reason))")
                // ... but one this app bundles must load
                let bundled = path == .mullion ? Wmtr.mullionLibraryPath : LauncherSettings.GraphicsPath.hostLibraryPath
                check(bundled == nil, "\(path.name): the bundled executor \(bundled ?? "-") does not load: \(reason)")
            } else if path != .vulkan, let want = o.wmtrLibrary {
                check(wmtr == "1", "\(path.name): STEAMAC_WMTR not set")
                check(lib == want, "\(path.name): STEAMAC_WMTR_LIB is \(lib ?? "unset"), not \(want)")
                if let lib, let h = dlopen(lib, RTLD_LAZY | RTLD_LOCAL) {
                    check(dlsym(h, "wmtr_host_create") != nil, "\(path.name): \(lib) has no wmtr_host_create")
                    dlclose(h)
                } else {
                    check(false, "\(path.name): cannot load \(lib ?? "-")")
                }
            } else {
                check(wmtr == nil && lib == nil, "\(path.name): wmtr environment left set")
            }
        }
        // Each path's executor: --graphics-path (a later --wmtr-lib names another build) and the settings. Mullion's
        // unix-call table is not DXMT's, so neither library may stand in for the other.
        check(Wmtr.mullionPath != Wmtr.path(for: ""), "Mullion's executor is DXMT's \(Wmtr.mullionPath)")
        let parsed: [([String], LauncherSettings.GraphicsPath, String?)] = [
            (["--graphics-path", "mullion"], .mullion, Wmtr.mullionPath),
            (["--graphics-path", "dxmt"], .dxmt, Wmtr.path(for: "")),
            (["--graphics-path", "vulkan"], .vulkan, nil),
            (["--graphics-path", "mullion", "--wmtr-lib", "/dev/libwmtr_host.dylib"], .mullion, "/dev/libwmtr_host.dylib"),
            (["--wmtr-lib", "/dev/libwmtr_host.dylib"], .dxmt, "/dev/libwmtr_host.dylib"),
            (["--graphics-path", "mullion", "--no-wmtr"], .vulkan, nil),
        ]
        for (args, want, library) in parsed {
            let o = try? Options.parse(["steamac-vm"] + args)
            check(o?.graphicsPath == want && o?.wmtrLibrary == library,
                  "\(args.joined(separator: " ")): \(o?.graphicsPath.rawValue ?? "error"), \(o?.wmtrLibrary ?? "no executor")")
        }
        let fromSettings: [(Bool, String, Bool, String?, Bool)] = [
            (true, "", true, Wmtr.mullionPath, true),             // Allow DXMT + mullion: Mullion's own executor
            (true, "/custom/libwmtr_host.dylib", true, Wmtr.mullionPath, true),  // a chosen DXMT executor is not Mullion's
            (true, "", false, Wmtr.path(for: ""), false),         // Allow DXMT: DXMT
            (false, "", true, nil, false),                        // mullion alone: Vulkan
        ]
        for (wmtr, library, mullion, wantLibrary, wantMullion) in fromSettings {
            let r = Options.wmtrSettings(wmtr: wmtr, library: library, mullion: mullion)
            check(r.library == wantLibrary && r.mullion == wantMullion,
                  "settings wmtr \(wmtr) library '\(library)' mullion \(mullion): \(r.library ?? "no executor"), mullion \(r.mullion)")
        }
        log("selftest-graphics-path: Mullion executor \(Wmtr.mullionLibraryPath ?? "not bundled (\(Wmtr.mullionPath))")")
        // Frame latency (Settings > Advanced): on maps to steamac.frame_latency=1, off adds no key;
        // the host side of the guest script's contract (scripts/test/steam-frame-latency.sh).
        check(Options.frameLatencyArg(true)?.key == "steamac.frame_latency" && Options.frameLatencyArg(true)?.value == "1",
              "frame latency on must map to steamac.frame_latency=1")
        check(Options.frameLatencyArg(false) == nil, "frame latency off must add no kernel key")
        check(Options().frameLatency == false, "frame latency must default off")
        log("selftest-graphics-path: frame latency \(Options.frameLatencyArg(true)?.key ?? "-")=\(Options.frameLatencyArg(true)?.value ?? "off")")
        log(failures == 0 ? "selftest-graphics-path: PASS" : "selftest-graphics-path: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}
