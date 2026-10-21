import Foundation

/// --selftest-graphics-path: the graphics path this launch resolved (Settings > Advanced "Allow DXMT", or
/// --wmtr-lib / --no-wmtr / --graphics-path) reaches the VM's kernel command line and the environment libkrun
/// reads, and the other path's environment is cleared. Run through `open` with only `defaults write ... wmtr`
/// changed, this shows that a plain double-click start enables the chosen path.
enum GraphicsPathSelfTest {
    static func run(_ resolved: Options) -> Never {
        var failures = 0
        func check(_ ok: Bool, _ what: String) { if !ok { failures += 1; log("selftest-graphics-path: FAIL \(what)") } }
        log("selftest-graphics-path: resolved \(resolved.graphicsPath.rawValue), cmdline \(resolved.cmdline)")
        check(resolved.cmdline.split(separator: " ").contains("steamac.gfx=\(resolved.graphicsPath.rawValue)"),
              "kernel command line lacks steamac.gfx=\(resolved.graphicsPath.rawValue)")
        for path in [resolved.graphicsPath] + LauncherSettings.GraphicsPath.allCases.filter({ $0 != resolved.graphicsPath }) {
            var o = resolved
            o.wmtrLibrary = path == .dxmt ? (resolved.wmtrLibrary ?? LauncherSettings.Wmtr.path(for: "")) : nil
            // left over from a shell or an earlier start: must not survive the Vulkan path
            setenv("STEAMAC_WMTR", "1", 1); setenv("STEAMAC_WMTR_LIB", "/nonexistent", 1)
            VM.applyGraphicsEnv(o)
            let wmtr = ProcessInfo.processInfo.environment["STEAMAC_WMTR"]
            let lib = ProcessInfo.processInfo.environment["STEAMAC_WMTR_LIB"]
            if path == .dxmt, let want = o.wmtrLibrary, let reason = LauncherSettings.Wmtr.unavailableReason(want) {
                // Options.resolve drops an executor that does not load, so a start never gets here with it
                log("selftest-graphics-path: DXMT executor not checked here (\(reason))")
            } else if path == .dxmt, let want = o.wmtrLibrary {
                check(wmtr == "1", "DXMT: STEAMAC_WMTR not set")
                check(lib == want, "DXMT: STEAMAC_WMTR_LIB is \(lib ?? "unset"), not \(want)")
                if let lib, let h = dlopen(lib, RTLD_LAZY | RTLD_LOCAL) {
                    check(dlsym(h, "wmtr_host_create") != nil, "DXMT: \(lib) has no wmtr_host_create")
                    dlclose(h)
                } else {
                    check(false, "DXMT: cannot load \(lib ?? "-")")
                }
            } else {
                check(wmtr == nil && lib == nil, "\(path.name): wmtr environment left set")
            }
        }
        // Frame latency (Settings > Advanced): on maps to steamac.frame_latency=1, off adds no key;
        // the host side of the guest script's contract (scripts/test/steam-frame-latency.sh).
        check(Options.frameLatencyArg(true)?.key == "steamac.frame_latency" && Options.frameLatencyArg(true)?.value == "1",
              "frame latency on must map to steamac.frame_latency=1")
        check(Options.frameLatencyArg(false) == nil, "frame latency off must add no kernel key")
        check(Options().frameLatency == false, "frame latency must default off")
        log("selftest-graphics-path: frame latency \(Options.frameLatencyArg(true)?.key ?? "-")=\(Options.frameLatencyArg(true)?.value ?? "off")")
        check(Options().experimentalGameSupport == false, "experimental game support must default off")
        for on in [true, false] {
            let a = Options.experimentalGameSupportArg(on)
            check(a.key == "steamac.fex_eac" && a.value == (on ? "1" : "0"), "experimental game support \(on) must map to steamac.fex_eac=\(on ? 1 : 0)")
        }
        check(Options.kernelArg("steamac.fex_eac", "1", unlessSetIn: "console=hvc0") == " steamac.fex_eac=1",
              "steamac.fex_eac must be added when --cmdline lacks it")
        check(Options.kernelArg("steamac.fex_eac", "1", unlessSetIn: "console=hvc0 steamac.fex_eac=0").isEmpty,
              "an explicit --cmdline steamac.fex_eac= must win")
        let fexWords = resolved.cmdline.split(separator: " ").filter { $0.hasPrefix("steamac.fex_eac=") }
        check(fexWords.count == 1, "kernel command line must carry steamac.fex_eac= once, has \(fexWords)")
        if !resolved.explicit.contains("--cmdline") {
            check(fexWords.map(String.init) == ["steamac.fex_eac=\(resolved.experimentalGameSupport ? 1 : 0)"],
                  "kernel command line \(fexWords) does not match experimental game support \(resolved.experimentalGameSupport)")
        }
        for (argv, want) in [(["--experimental-game-support"], true), (["--no-experimental-game-support"], false),
                             (["--no-experimental-game-support", "--experimental-game-support"], true),
                             (["--experimental-game-support", "--no-experimental-game-support"], false)] {
            check((try? Options.parse(["steamac-vm"] + argv))?.experimentalGameSupport == want, "\(argv) must give experimental game support \(want)")
        }
        log("selftest-graphics-path: experimental game support \(fexWords.joined(separator: " "))")
        log(failures == 0 ? "selftest-graphics-path: PASS" : "selftest-graphics-path: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}
