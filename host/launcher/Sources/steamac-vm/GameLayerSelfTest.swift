import AppKit
import QuartzCore

/// --selftest-game-layers: game layers cover the guest picture only while a game has focus.
/// The picture and non-Metal sublayers are untouched; the HUD follows the game/picture policy.
enum GameLayerSelfTest {
    static func run() -> Never {
        let parent = CAMetalLayer()
        let game = CAMetalLayer(), other = CALayer()
        parent.addSublayer(game)
        parent.addSublayer(other)
        var failures = 0
        func check(_ ok: Bool, _ what: String) {
            if !ok { failures += 1; log("selftest-game-layers: FAIL \(what)") }
        }
        VMView.applyGameLayers(visible: false, in: parent)
        check(game.isHidden, "game layer visible while the guest shows Steam")
        check(!other.isHidden && !parent.isHidden, "non-game layers hidden")
        check(game.animationKeys()?.isEmpty ?? true, "visibility change animated")
        VMView.applyGameLayers(visible: true, in: parent)
        check(!game.isHidden, "game layer hidden while the game has focus")
        func mode(_ layer: CAMetalLayer) -> String? { layer.developerHUDProperties?["mode"] as? String }
        VMView.applyHUD(true, gameVisible: true, main: parent, mainAttached: true)
        check(mode(game) == "default" && mode(parent) == "off", "HUD on with a game: not on the game layer only")
        VMView.applyHUD(true, gameVisible: false, main: parent, mainAttached: true)
        check(mode(game) == "off" && mode(parent) == "default", "HUD on without a game: not on the guest picture only")
        VMView.applyHUD(false, gameVisible: true, main: parent, mainAttached: true)
        check(mode(game) == "off" && mode(parent) == "off", "HUD off: still shown")
        let bare = CAMetalLayer()
        VMView.applyHUD(true, gameVisible: true, main: bare, mainAttached: true)
        check(mode(bare) == "default", "HUD on without game layers: not on the guest picture")
        let unattached = CAMetalLayer()
        VMView.applyHUD(false, gameVisible: false, main: unattached, mainAttached: false)
        check(mode(unattached) == nil, "picture HUD configured before its first on-screen drawable")
        PointerInput.configure()
        check(!NSEvent.isMouseCoalescingEnabled, "mouse coalescing still on after PointerInput.configure")
        log(failures == 0 ? "selftest-game-layers: PASS" : "selftest-game-layers: \(failures) FAILED")
        exit(failures == 0 ? 0 : 1)
    }
}
