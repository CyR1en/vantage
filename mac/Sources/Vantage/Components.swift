import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// Finder icons, cached by type so large lists stay fast.
@MainActor
enum IconCache {
    private static var byKey: [String: NSImage] = [:]

    static func icon(tree: ScanTree, item: Int) -> NSImage {
        let name = tree.name(item)
        let ext = (name as NSString).pathExtension.lowercased()
        if tree.isDirectory[item] {
            // Packages such as apps have their own icons.
            if !ext.isEmpty, let type = UTType(filenameExtension: ext), type.conforms(to: .package) {
                return NSWorkspace.shared.icon(forFile: tree.url(item).path)
            }
            return cached("/folder") { NSWorkspace.shared.icon(for: .folder) }
        }
        return cached(ext) {
            NSWorkspace.shared.icon(for: UTType(filenameExtension: ext) ?? .data)
        }
    }

    private static func cached(_ key: String, _ make: () -> NSImage) -> NSImage {
        if let image = byKey[key] { return image }
        let image = make()
        byKey[key] = image
        return image
    }
}

struct ItemIcon: View {
    let tree: ScanTree
    let item: Int
    var size: CGFloat = 16

    var body: some View {
        Image(nsImage: IconCache.icon(tree: tree, item: item))
            .resizable()
            .interpolation(.high)
            .frame(width: size, height: size)
    }
}

/// A slim proportional bar, tinted by the item's kind.
struct SizeBar: View {
    let fraction: Double
    let color: Color

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(.quaternary)
                Capsule()
                    .fill(color.gradient)
                    .frame(width: max(fraction > 0 ? 3 : 0, proxy.size.width * min(max(fraction, 0), 1)))
            }
        }
        .frame(height: 6)
    }
}

/// Actions for one or more items, shared by the list, treemap, and menus.
struct ItemMenu: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    let items: [Int]

    var body: some View {
        if model.browser === browser, !items.isEmpty {
            let tree = browser.tree
            let single = items.count == 1 ? items[0] : nil
            if let single, tree.isDirectory[single] {
                Button("Open", systemImage: "arrow.right.circle") { model.navigate(to: single) }
            }
            if let single {
                Button("Quick Look", systemImage: "eye") { model.quickLook(single) }
            }
            Button("Open with Default App", systemImage: "arrow.up.forward.app") { model.openWithDefaultApp(items) }
            Button("Show in Finder", systemImage: "finder") { model.reveal(items) }
            if let single, model.sidebar != .folders {
                Button("Show in Folder View", systemImage: "folder") { model.showInFolder(single) }
            }
            Divider()
            Button("Copy Path", systemImage: "document.on.document") { model.copyPaths(items) }
            Divider()
            Button("Move to Trash", systemImage: "trash") { model.requestDeletion(items) }
            Button("Delete Immediately…", systemImage: "xmark.bin", role: .destructive) {
                model.requestDeletion(items, permanently: true)
            }
        }
    }
}
