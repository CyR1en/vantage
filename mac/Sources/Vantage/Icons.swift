import AppKit
import SwiftUI

/// A Phosphor icon (phosphoricons.com, MIT) compiled into the app from
/// `mac/Support/Icons.xcassets`. Each names the SF Symbol drawn in its place when
/// the catalog is absent, as in `swift run` and tests. Add icons to
/// `mac/Support/Icons.txt` and run `mac/update-icons.sh`.
struct Icon: Hashable, Sendable {
    let name: String
    let fallback: String
    /// Phosphor gives bare glyphs (plus, x, check, lists) a duotone layer that is
    /// just a square behind them, which reads as a stray box; these draw bold instead.
    var boxedDuotone = false

    /// Regular for idle controls, bold for small or emphasized glyphs, duotone for
    /// selected and active states (bold for `boxedDuotone` icons), fill for status marks.
    enum Weight: String, Sendable { case regular, bold, fill, duotone }

    // Navigation and chrome
    static let back = Icon(name: "caret-left", fallback: "chevron.left")
    static let forward = Icon(name: "caret-right", fallback: "chevron.right")
    static let caretUp = Icon(name: "caret-up", fallback: "chevron.up")
    static let caretDown = Icon(name: "caret-down", fallback: "chevron.down")
    static let sidebar = Icon(name: "sidebar-simple", fallback: "sidebar.left")
    static let search = Icon(name: "magnifying-glass", fallback: "magnifyingglass")
    static let close = Icon(name: "x", fallback: "xmark", boxedDuotone: true)
    static let clear = Icon(name: "x-circle", fallback: "xmark.circle.fill")
    static let settings = Icon(name: "gear-six", fallback: "gearshape")
    static let add = Icon(name: "plus", fallback: "plus", boxedDuotone: true)

    // Views and actions
    static let folders = Icon(name: "folder", fallback: "folder")
    static let largestFiles = Icon(name: "list-numbers", fallback: "list.number", boxedDuotone: true)
    static let quickLook = Icon(name: "eye", fallback: "eye")
    static let reveal = Icon(name: "folder-open", fallback: "folder")
    static let trash = Icon(name: "trash", fallback: "trash")
    static let treemap = Icon(name: "layout", fallback: "square.grid.3x3.square")
    static let rescan = Icon(name: "arrow-clockwise", fallback: "arrow.clockwise")
    static let chooseFolder = Icon(name: "folder-plus", fallback: "folder.badge.plus")
    static let summary = Icon(name: "chart-pie-slice", fallback: "chart.pie.fill")
    static let selection = Icon(name: "stack", fallback: "square.stack.3d.up.fill")
    static let empty = Icon(name: "selection", fallback: "square.dashed")

    // Places
    static let home = Icon(name: "house", fallback: "house.fill")
    static let desktop = Icon(name: "desktop", fallback: "menubar.dock.rectangle")
    static let documents = Icon(name: "files", fallback: "doc.fill")
    static let downloads = Icon(name: "download", fallback: "arrow.down.circle.fill")
    static let applications = Icon(name: "squares-four", fallback: "square.grid.3x3.fill")
    static let disk = Icon(name: "hard-drive", fallback: "internaldrive.fill")
    static let disks = Icon(name: "hard-drives", fallback: "internaldrive.fill")

    // Status
    static let done = Icon(name: "check-circle", fallback: "checkmark.circle.fill")
    static let check = Icon(name: "check", fallback: "checkmark", boxedDuotone: true)
    static let verified = Icon(name: "seal-check", fallback: "checkmark.seal.fill")
    static let warning = Icon(name: "warning", fallback: "exclamationmark.triangle.fill")
    static let protected = Icon(name: "shield-check", fallback: "lock.shield.fill")
    static let privacy = Icon(name: "hand", fallback: "hand.raised.fill")
    static let toggle = Icon(name: "toggle-right", fallback: "switch.2")
    static let help = Icon(name: "question", fallback: "questionmark.circle")

    fileprivate func assetName(_ weight: Weight) -> String { "ph.\(name).\(weight.rawValue)" }
}

@MainActor
private enum IconCatalog {
    private static var images: [String: NSImage?] = [:]

    static func image(_ icon: Icon, _ weight: Icon.Weight) -> NSImage? {
        let key = icon.assetName(weight)
        if let cached = images[key] { return cached }
        let image = NSImage(named: key)
        images[key] = image
        return image
    }
}

/// Draws an `Icon` as a template image at a fixed size, tinted by the foreground style.
struct IconImage: View {
    let icon: Icon
    var weight: Icon.Weight = .regular
    var size: CGFloat = 16

    init(_ icon: Icon, weight: Icon.Weight = .regular, size: CGFloat = 16) {
        self.icon = icon
        self.weight = weight == .duotone && icon.boxedDuotone ? .bold : weight
        self.size = size
    }

    var body: some View {
        if let image = IconCatalog.image(icon, weight) {
            Image(nsImage: image)
                .renderingMode(.template)
                .resizable()
                .interpolation(.high)
                .aspectRatio(contentMode: .fit)
                .frame(width: size, height: size)
                .accessibilityHidden(true)
        } else {
            Image(systemName: icon.fallback)
                .font(.system(size: size * 0.78, weight: weight == .bold ? .semibold : .regular))
                .frame(width: size, height: size)
                .accessibilityHidden(true)
        }
    }
}

extension Label where Title == Text, Icon == IconImage {
    /// A label with a Phosphor icon, for buttons, toolbars, and status text.
    init(_ title: String, icon: Vantage.Icon, weight: Vantage.Icon.Weight = .regular, size: CGFloat = 16) {
        self.init { Text(title) } icon: { IconImage(icon, weight: weight, size: size) }
    }
}
