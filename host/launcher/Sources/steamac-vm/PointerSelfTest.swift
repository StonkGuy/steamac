import AppKit
import Foundation

/// `--selftest-pointer`: the `auto` mouse mode's host pointer -> guest cursor mapping. Part 1 checks
/// PointerMap geometry: Retina off/on, window == / != scanout size (pending resize, fixed mode),
/// full-screen letterbox, a smaller surface at gamescope's max scale, and Steam's UI at a
/// non-native Display > Resolution (STEAMAC-2V/2W). Part 2 drives real WindowControllers through
/// BootProgress focus lines (`focus steam 1920x1200`, `focus game …`, `focus desktop …`) and
/// scanout resizes, with MetalFX on and off, and checks where a view point lands in the guest.
enum PointerSelfTest {
    static func run() -> Never {
        _ = SteamacApplication.shared
        var checks = 0, failures = 0
        func expect(_ name: String, _ got: (Int, Int), _ want: (Int, Int)) {
            checks += 1
            let ok = got == want
            if !ok { failures += 1 }
            print("pointer selftest: \(ok ? "PASS" : "FAIL") \(name): \(got.0),\(got.1)\(ok ? "" : " (expected \(want.0),\(want.1))")")
        }
        /// View point given from the top-left (as on screen) in a view `height` points tall.
        func point(_ x: Double, _ yTop: Double, _ height: Double) -> CGPoint { CGPoint(x: x, y: height - yTop) }

        // Part 1: geometry. `view` is the view size in points, `frame` the presented frame.
        func map(view: CGSize, frame: (Int, Int), display: (Int, Int), surface: (Int, Int)? = nil) -> PointerMap {
            let picture = Renderer.fit(content: CGSize(width: frame.0, height: frame.1), in: CGRect(origin: .zero, size: view))
            return PointerMap(picture: picture, display: display, surface: surface ?? display)
        }
        do {
            let m = map(view: CGSize(width: 1440, height: 900), frame: (1440, 900), display: (1440, 900))
            expect("retina off, window == scanout: centre", m.surfacePoint(point(720, 450, 900)), (720, 450))
            expect("retina off, window == scanout: 1/4 width", m.surfacePoint(point(360, 450, 900)), (360, 450))
        }
        do {
            let m = map(view: CGSize(width: 1440, height: 900), frame: (2880, 1800), display: (2880, 1800))
            expect("retina on, window == scanout: centre", m.surfacePoint(point(720, 450, 900)), (1440, 900))
            expect("retina on, window == scanout: 1/4 width", m.surfacePoint(point(360, 450, 900)), (720, 900))
            expect("retina on, window == scanout: +1 pt = +2 px", m.surfacePoint(point(361, 450, 900)), (722, 900))
        }
        do {
            // STEAMAC-2V: Steam's Display > Resolution 1920x1200 on a 2880x1800 Retina display.
            let m = map(view: CGSize(width: 1440, height: 900), frame: (2880, 1800), display: (2880, 1800), surface: (1920, 1200))
            expect("retina on, Steam UI 1920x1200: centre", m.surfacePoint(point(720, 450, 900)), (960, 600))
            expect("retina on, Steam UI 1920x1200: 1/4 width", m.surfacePoint(point(360, 450, 900)), (480, 600))
            expect("retina on, Steam UI 1920x1200: right edge", m.surfacePoint(point(1439.5, 450, 900)), (1919, 600))
        }
        do {
            // STEAMAC-2W: window 1774x1079 pt (3548x2158 px), Steam UI 1920x1080 (shown 3548x1995).
            let m = map(view: CGSize(width: 1774, height: 1079), frame: (3548, 2158), display: (3548, 2158), surface: (1920, 1080))
            expect("retina on, Steam UI 1920x1080 in 3548x2158: centre", m.surfacePoint(point(887, 539.5, 1079)), (960, 540))
            expect("retina on, Steam UI 1920x1080 in 3548x2158: 1/4 width", m.surfacePoint(point(444, 539.5, 1079)), (480, 540))
            expect("retina on, Steam UI 1920x1080 in 3548x2158: bar above clamps", m.surfacePoint(point(0, 20, 1079)), (0, 0))
        }
        do {
            // Pending resize / fixed display mode: 2880x1800 frames in a 1600x900 pt window.
            let m = map(view: CGSize(width: 1600, height: 900), frame: (2880, 1800), display: (2880, 1800))
            expect("retina on, window != scanout: centre", m.surfacePoint(point(800, 450, 900)), (1440, 900))
            expect("retina on, window != scanout: picture top-left", m.surfacePoint(point(80, 0, 900)), (0, 0))
            expect("retina on, window != scanout: pillarbox clamps", m.surfacePoint(point(40, 450, 900)), (0, 900))
            let off = map(view: CGSize(width: 1600, height: 900), frame: (1440, 900), display: (1440, 900))
            expect("retina off, window != scanout: centre", off.surfacePoint(point(800, 450, 900)), (720, 450))
        }
        do {
            // Full screen on a 1728x1117 pt screen with a 2880x1800 scanout: letterboxed 1728x1080.
            let m = map(view: CGSize(width: 1728, height: 1117), frame: (2880, 1800), display: (2880, 1800))
            expect("full screen letterbox: centre", m.surfacePoint(point(864, 558.5, 1117)), (1440, 900))
            expect("full screen letterbox: picture top-left", m.surfacePoint(point(0, 18.5, 1117)), (0, 0))
            expect("full screen letterbox: +1 pt", m.surfacePoint(point(865, 558.5, 1117)), (1441, 900))
            let steam = map(view: CGSize(width: 1728, height: 1117), frame: (2880, 1800), display: (2880, 1800), surface: (1920, 1200))
            expect("full screen letterbox, Steam UI 1920x1200: centre", steam.surfacePoint(point(864, 558.5, 1117)), (960, 600))
            expect("full screen letterbox, Steam UI 1920x1200: 1/4 width", steam.surfacePoint(point(432, 558.5, 1117)), (480, 600))
        }
        do {
            // A 640x400 surface on a 1440x900 display: gamescope upscales at most 2x (1280x800, centred).
            let m = map(view: CGSize(width: 1440, height: 900), frame: (1440, 900), display: (1440, 900), surface: (640, 400))
            expect("max-scale 2 surface: centre", m.surfacePoint(point(720, 450, 900)), (320, 200))
            expect("max-scale 2 surface: top-left", m.surfacePoint(point(80, 50, 900)), (0, 0))
        }

        // Part 2: real WindowController + BootProgress focus lines.
        func controller(points: CGSize, scale: Double) -> (WindowController, BootProgress) {
            let wc = WindowController(title: "pointer selftest", width: Int(points.width), height: Int(points.height),
                                      pixelScale: scale, renderer: Renderer(), inputs: nil, mouseMode: .auto)
            wc.window.setContentSize(points)
            let (w, h) = wc.guestSizeForWindow()
            wc.scanoutResized(width: w, height: h)
            let progress = BootProgress()
            wc.attach(progress: progress)
            return (wc, progress)
        }
        for metalFX in [false, true] {
            let fx = metalFX ? ", MetalFX" : ""
            let (wc, progress) = controller(points: CGSize(width: 1200, height: 750), scale: 2)
            wc.view.renderer.superResolution = metalFX
            func at(_ x: Double, _ yTop: Double) -> (Int, Int) {
                wc.pointerMap.surfacePoint(point(x, yTop, Double(wc.view.bounds.height)))
            }
            let b = wc.view.bounds.size
            guard b == CGSize(width: 1200, height: 750) else {
                print("pointer selftest: FAIL window content \(b), expected 1200x750"); exit(1)
            }
            progress.guestLine("focus steam")
            expect("wc retina\(fx), Steam at display size: centre", at(600, 375), (1200, 750))
            progress.guestLine("focus steam 1920x1200")
            expect("wc retina\(fx), `focus steam 1920x1200`: centre", at(600, 375), (960, 600))
            expect("wc retina\(fx), `focus steam 1920x1200`: 1/4 width", at(300, 375), (480, 600))
            progress.guestLine("focus game 4242 1600x900")
            expect("wc retina\(fx), `focus game 4242 1600x900`: centre", at(600, 375), (800, 450))
            expect("wc retina\(fx), `focus game 4242 1600x900`: surface top-left", at(0, 37.5), (0, 0))
            progress.guestLine("focus desktop 1280x800")
            expect("wc retina\(fx), `focus desktop 1280x800`: 1/4 width", at(300, 375), (320, 400))
            progress.guestLine("focus desktop")
            expect("wc retina\(fx), session ended: display again", at(300, 375), (600, 750))
            // The guest switched to another mode (pending resize; same aspect), then a 4:3 fixed mode.
            progress.guestLine("focus steam 1920x1200")
            wc.scanoutResized(width: 2880, height: 1800)
            expect("wc retina\(fx), scanout 2880x1800 in a 2400x1500 window: 1/4 width", at(300, 375), (480, 600))
            wc.scanoutResized(width: 2400, height: 1800)
            expect("wc retina\(fx), 4:3 scanout, Steam 1920x1200: centre", at(600, 375), (960, 600))
            expect("wc retina\(fx), 4:3 scanout, Steam 1920x1200: 1/4 of the picture", at(350, 375), (480, 600))
            wc.window.close()
        }
        do {
            let (wc, progress) = controller(points: CGSize(width: 1200, height: 750), scale: 1)
            func at(_ x: Double, _ yTop: Double) -> (Int, Int) {
                wc.pointerMap.surfacePoint(point(x, yTop, Double(wc.view.bounds.height)))
            }
            progress.guestLine("focus steam")
            expect("wc retina off, Steam at display size: 1/4 width", at(300, 375), (300, 375))
            progress.guestLine("focus steam 960x600")
            expect("wc retina off, `focus steam 960x600`: 1/4 width", at(300, 375), (240, 300))
            wc.window.close()
        }
        print("pointer selftest: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
