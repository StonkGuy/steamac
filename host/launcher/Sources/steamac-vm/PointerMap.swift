import CoreGraphics

/// Host pointer -> guest cursor mapping of the `auto` mouse mode (WindowController, MARK: pointer).
/// gamescope moves its cursor by relative motion in the pixels of the focused surface (Steam's
/// window, a game's, the Plasma desktop) and shows that surface fitted into the display, so a view
/// point maps through the picture rect onto the surface, never onto the display's pixels: they
/// differ whenever Steam's Settings > Display > Resolution is not the native one (Steam sets that
/// Xwayland mode, e.g. a 1920x1200 UI on a 2880x1800 display, STEAMAC-2V/2W).
struct PointerMap {
    /// gamescope's `--max-scale` (guest/layer/usr/lib/steamos/gamescope-session): a smaller
    /// surface is shown at most this many times its size.
    static let gamescopeMaxScale = 2.0

    /// The guest picture in view points: the presented frame aspect-fit into the view
    /// (VMView.fitRect; the same rect with MetalFX, which only upscales inside it).
    var picture: CGRect
    /// The display (scanout) size in guest pixels.
    var display: (Int, Int)
    /// The surface the cursor moves on, in its pixels.
    var surface: (Int, Int)

    /// Where `surface` appears in the view: gamescope fits it into the display (aspect kept,
    /// centred, upscaled at most `gamescopeMaxScale` times).
    var surfaceRect: CGRect {
        let (w, h) = surface, (dw, dh) = display
        guard (w, h) != (dw, dh), w > 0, h > 0, dw > 0, dh > 0 else { return picture }
        let scale = min(Double(dw) / Double(w), Double(dh) / Double(h), PointerMap.gamescopeMaxScale)
        let pw = picture.width * CGFloat(Double(w) * scale / Double(dw))
        let ph = picture.height * CGFloat(Double(h) * scale / Double(dh))
        return CGRect(x: picture.midX - pw / 2, y: picture.midY - ph / 2, width: pw, height: ph)
    }

    /// View point (AppKit: bottom-left origin) -> surface pixel (top-left origin), clamped to it.
    func surfacePoint(_ p: CGPoint) -> (Int, Int) {
        let r = surfaceRect
        guard r.width > 0, r.height > 0 else { return (0, 0) }
        let ux = Double((p.x - r.minX) / r.width)
        let uy = 1 - Double((p.y - r.minY) / r.height)
        let (w, h) = surface
        return (min(w - 1, max(0, Int(ux * Double(w)))), min(h - 1, max(0, Int(uy * Double(h)))))
    }
}
