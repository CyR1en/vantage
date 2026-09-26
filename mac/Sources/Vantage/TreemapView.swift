import SwiftUI

struct TreemapTile {
    let item: Int
    let rect: CGRect
    let depth: Int
    let top: Int            // the direct child of the displayed folder containing this tile
    let isContainer: Bool   // a folder whose contents are drawn inside it
}

enum TreemapLayout {
    static let header: CGFloat = 18
    static let gap: CGFloat = 2

    static func tiles(tree: ScanTree, directory: Int, in bounds: CGRect) -> [TreemapTile] {
        var out: [TreemapTile] = []
        layout(tree: tree, directory: directory, rect: bounds, depth: 0, top: nil, into: &out)
        return out
    }

    private static func layout(tree: ScanTree, directory: Int, rect: CGRect, depth: Int, top: Int?, into out: inout [TreemapTile]) {
        let total = Double(tree.size[directory])
        guard total > 0, rect.width > 3, rect.height > 3 else { return }
        var items: [(Int, Double)] = []
        let area = Double(rect.width * rect.height)
        var shown = 0.0
        for child in tree.children(of: directory, limit: 400) {
            let s = Double(tree.size[child])
            guard s > 0 else { break }        // children are sorted by size
            // Keep tiles that would be at least a few pixels; lump the rest together.
            if s / total * area < 9 && items.count > 0 { break }
            items.append((child, s))
            shown += s
            if items.count >= 400 { break }
        }
        guard !items.isEmpty else { return }
        // Scale so the visible tiles fill the rectangle proportionally to the whole folder.
        let scale = area / total
        let remaining = items.map { (item: $0.0, area: $0.1 * scale) }
        let leftover = (total - shown) * scale
        var free = rect
        if leftover > 0 {
            // Reserve space for tiny items so proportions stay honest.
            if free.width >= free.height {
                free.size.width -= CGFloat(leftover) / free.height
            } else {
                free.size.height -= CGFloat(leftover) / free.width
            }
        }
        var first = 0
        while first < remaining.count {
            let side = Double(min(free.width, free.height))
            var rowArea = remaining[first].area
            let maxArea = rowArea   // input areas are sorted largest first
            var minArea = rowArea
            var k = first + 1
            while k < remaining.count {
                let next = remaining[k].area
                if worst(sum: rowArea + next, min: next, max: maxArea, side: side)
                    > worst(sum: rowArea, min: minArea, max: maxArea, side: side) { break }
                rowArea += next
                minArea = next
                k += 1
            }
            let row = remaining[first..<k]
            first = k
            if free.width >= free.height {
                let w = CGFloat(rowArea / Double(free.height))
                var y = free.minY
                for entry in row {
                    let h = CGFloat(entry.area) / w
                    emit(tree: tree, item: entry.item, rect: CGRect(x: free.minX, y: y, width: w, height: h), depth: depth, top: top, into: &out)
                    y += h
                }
                free.origin.x += w; free.size.width -= w
            } else {
                let h = CGFloat(rowArea / Double(free.width))
                var x = free.minX
                for entry in row {
                    let w = CGFloat(entry.area) / h
                    emit(tree: tree, item: entry.item, rect: CGRect(x: x, y: free.minY, width: w, height: h), depth: depth, top: top, into: &out)
                    x += w
                }
                free.origin.y += h; free.size.height -= h
            }
        }
    }

    private static func emit(tree: ScanTree, item: Int, rect: CGRect, depth: Int, top: Int?, into out: inout [TreemapTile]) {
        let tile = rect.insetBy(dx: gap / 2, dy: gap / 2)
        guard tile.width >= 1, tile.height >= 1 else { return }
        let topItem = top ?? item
        let nests = tree.isDirectory[item] && depth < 3 && tile.width > 40 && tile.height > header + 24
            && tree.size[item] > 0 && tree.items[item] > 0
        out.append(TreemapTile(item: item, rect: tile, depth: depth, top: topItem, isContainer: nests))
        if nests {
            let inner = CGRect(x: tile.minX + 3, y: tile.minY + header, width: tile.width - 6, height: tile.height - header - 3)
            layout(tree: tree, directory: item, rect: inner, depth: depth + 1, top: topItem, into: &out)
        }
    }

    private static func worst(sum: Double, min minA: Double, max maxA: Double, side: Double) -> Double {
        guard sum > 0, side > 0 else { return .infinity }
        let s2 = side * side, sum2 = sum * sum
        return max(s2 * maxA / sum2, sum2 / (s2 * minA))
    }
}

struct TreemapView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.colorScheme) private var colorScheme
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }

    @State private var tiles: [TreemapTile] = []
    @State private var layoutKey: LayoutKey?
    @State private var hovered: Int?
    @State private var hoverPoint: CGPoint = .zero
    @State private var morph: Morph?

    private struct LayoutKey: Equatable {
        var directory: Int
        var revision: Int
        var size: CGSize
    }

    /// Where tiles were drawn when a resize replaced the layout; they ease from there to their new rects.
    private struct Morph {
        var from: [Int: CGRect]
        var start: Date
    }

    private static let morphDuration = 0.22

    var body: some View {
        GeometryReader { proxy in
            let key = LayoutKey(directory: browser.directory, revision: browser.revision, size: proxy.size)
            ZStack(alignment: .topLeading) {
                TimelineView(.animation(paused: morph == nil)) { timeline in
                    Canvas(opaque: false, rendersAsynchronously: false) { context, size in
                        draw(in: &context, size: size, at: timeline.date)
                    }
                }
                .onContinuousHover(coordinateSpace: .local) { phase in
                    switch phase {
                    case .active(let point):
                        hoverPoint = point
                        hovered = tile(at: point)?.item
                    case .ended:
                        hovered = nil
                    }
                }
                .gesture(SpatialTapGesture(count: 2).onEnded { value in
                    if let tile = tile(at: value.location) { model.open(tile.item) }
                })
                .simultaneousGesture(SpatialTapGesture(count: 1).onEnded { value in
                    if let tile = tile(at: value.location) {
                        browser.selection = [tile.item]
                    } else {
                        browser.selection = []
                    }
                })
                .contextMenu {
                    if let item = hovered {
                        ItemMenu(items: [item])
                    }
                }

                if let item = hovered, !tree.isRemoved(item) {
                    HoverCard(tree: tree, item: item, parentSize: tree.size[Int(tree.parent[item])])
                        .fixedSize()
                        .offset(cardOffset(in: proxy.size))
                        .allowsHitTesting(false)
                        .transition(.opacity)
                }

                if tiles.isEmpty {
                    ContentUnavailableView {
                        Label("Nothing to Show", icon: .empty, weight: .duotone, size: 48)
                    } description: {
                        Text("This folder doesn’t contain any files with a size.")
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .onAppear { relayout(key) }
            .onChange(of: key) { _, new in relayout(new) }
            .task(id: morph?.start) {
                guard morph != nil else { return }
                try? await Task.sleep(for: .seconds(Self.morphDuration))
                if !Task.isCancelled { morph = nil }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Treemap of \(tree.name(browser.directory))")
    }

    private func relayout(_ key: LayoutKey) {
        guard layoutKey != key else { return }
        // A resize of the same folder morphs from the tiles as last drawn, stretched to the new
        // bounds, so only squarify's rearrangements animate. Other changes replace the tiles outright.
        if let old = layoutKey, old.directory == key.directory, old.revision == key.revision,
           old.size.width > 0, old.size.height > 0, !tiles.isEmpty {
            let now = Date()
            let sx = key.size.width / old.size.width, sy = key.size.height / old.size.height
            var from: [Int: CGRect] = [:]
            from.reserveCapacity(tiles.count)
            for tile in tiles {
                let r = displayedRect(tile, progress: progress(at: now)).rect
                from[tile.item] = CGRect(x: r.minX * sx, y: r.minY * sy, width: r.width * sx, height: r.height * sy)
            }
            morph = Morph(from: from, start: now)
        } else {
            morph = nil
        }
        layoutKey = key
        tiles = TreemapLayout.tiles(tree: tree, directory: key.directory, in: CGRect(origin: .zero, size: key.size).insetBy(dx: 1, dy: 1))
    }

    /// Eased morph progress, or nil when tiles sit at their laid-out rects.
    private func progress(at date: Date) -> Double? {
        guard let morph else { return nil }
        let t = date.timeIntervalSince(morph.start) / Self.morphDuration
        guard t < 1 else { return nil }
        return 1 - pow(1 - max(t, 0), 3)
    }

    private func displayedRect(_ tile: TreemapTile, progress: Double?) -> (rect: CGRect, opacity: Double) {
        guard let progress, let morph else { return (tile.rect, 1) }
        // Tiles new to this layout (small items, newly nested contents) fade in where they belong.
        guard let from = morph.from[tile.item] else { return (tile.rect, progress) }
        let to = tile.rect
        func mix(_ a: CGFloat, _ b: CGFloat) -> CGFloat { a + (b - a) * progress }
        return (CGRect(x: mix(from.minX, to.minX), y: mix(from.minY, to.minY),
                       width: mix(from.width, to.width), height: mix(from.height, to.height)), 1)
    }

    private func tile(at point: CGPoint) -> TreemapTile? {
        tiles.last { $0.rect.contains(point) }
    }

    private func cardOffset(in size: CGSize) -> CGSize {
        let w: CGFloat = 240, h: CGFloat = 80
        var x = hoverPoint.x + 16, y = hoverPoint.y + 16
        if x + w > size.width { x = hoverPoint.x - w - 8 }
        if y + h > size.height { y = hoverPoint.y - h - 8 }
        return CGSize(width: max(4, x), height: max(4, y))
    }

    private func color(for tile: TreemapTile) -> Color {
        tree.category[tile.item].color
    }

    private func draw(in context: inout GraphicsContext, size: CGSize, at date: Date) {
        let dark = colorScheme == .dark
        let selected = browser.selection
        let progress = progress(at: date)
        // The canvas can be drawn at a new size a frame before relayout runs; stretch rather than misfit.
        let laidOut = layoutKey?.size ?? size
        let sx = laidOut.width > 0 ? size.width / laidOut.width : 1
        let sy = laidOut.height > 0 ? size.height / laidOut.height : 1
        let stretch = sx != 1 || sy != 1
        for tile in tiles {
            var (rect, opacity) = displayedRect(tile, progress: progress)
            if stretch { rect = CGRect(x: rect.minX * sx, y: rect.minY * sy, width: rect.width * sx, height: rect.height * sy) }
            context.opacity = opacity
            // Outer tiles follow the card's corners (card radius less its padding); nested ones step in.
            let radius: CGFloat = max(Chrome.cardRadius - 8 - 2 * CGFloat(tile.depth), 3)
            let path = Path(roundedRect: rect, cornerRadius: min(radius, rect.width / 3, rect.height / 3), style: .continuous)
            let base = color(for: tile)
            if tile.isContainer {
                context.fill(path, with: .color(base.opacity(dark ? 0.13 + 0.05 * Double(tile.depth) : 0.10 + 0.05 * Double(tile.depth))))
            } else {
                let gradient = Gradient(colors: [base.opacity(dark ? 0.92 : 0.88), base.opacity(dark ? 0.66 : 0.7)])
                context.fill(path, with: .linearGradient(gradient, startPoint: rect.origin, endPoint: CGPoint(x: rect.maxX, y: rect.maxY)))
                // A soft top-lit rim, like the glass around it.
                if rect.width > 6, rect.height > 6 {
                    let rim = Path(roundedRect: rect.insetBy(dx: 0.5, dy: 0.5),
                                   cornerRadius: max(min(radius, rect.width / 3, rect.height / 3) - 0.5, 0), style: .continuous)
                    context.stroke(rim, with: .linearGradient(Gradient(colors: [.white.opacity(dark ? 0.22 : 0.35), .white.opacity(0)]),
                                                              startPoint: rect.origin,
                                                              endPoint: CGPoint(x: rect.minX, y: rect.maxY)),
                                   lineWidth: 1)
                }
            }
            if hovered == tile.item {
                context.fill(path, with: .color(.white.opacity(0.18)))
            }
            if selected.contains(tile.item) {
                context.stroke(path, with: .color(.accentColor), lineWidth: 2.5)
                context.stroke(Path(roundedRect: rect.insetBy(dx: 2, dy: 2), cornerRadius: max(radius - 2, 1), style: .continuous),
                               with: .color(.white.opacity(0.8)), lineWidth: 1)
            }
            drawLabel(tile, rect: rect, in: &context, dark: dark)
        }
    }

    private func drawLabel(_ tile: TreemapTile, rect r: CGRect, in context: inout GraphicsContext, dark: Bool) {
        guard r.width > 44, r.height > 16 else { return }
        let name = tree.name(tile.item)
        if tile.isContainer {
            let label = "\(name)  \(Format.bytes(tree.size[tile.item]))"
            let text = context.resolve(Text(label).font(.system(size: 11, weight: .semibold))
                .foregroundStyle(dark ? Color.white.opacity(0.88) : Color.black.opacity(0.75)))
            context.drawLayer { layer in
                layer.clip(to: Path(CGRect(x: r.minX + 6, y: r.minY, width: r.width - 12, height: TreemapLayout.header)))
                layer.draw(text, at: CGPoint(x: r.minX + 7, y: r.minY + 2), anchor: .topLeading)
            }
        } else if r.height > 30 {
            let title = context.resolve(Text(name).font(.system(size: 11, weight: .semibold)).foregroundStyle(.white))
            context.drawLayer { layer in
                layer.clip(to: Path(r.insetBy(dx: 5, dy: 3)))
                layer.addFilter(.shadow(color: .black.opacity(0.35), radius: 1, y: 0.5))
                layer.draw(title, at: CGPoint(x: r.minX + 6, y: r.minY + 5), anchor: .topLeading)
                if r.height > 44 {
                    let size = layer.resolve(Text(Format.bytes(tree.size[tile.item])).font(.system(size: 10, weight: .medium)).foregroundStyle(.white.opacity(0.85)))
                    layer.draw(size, at: CGPoint(x: r.minX + 6, y: r.minY + 20), anchor: .topLeading)
                }
            }
        }
    }
}

private struct HoverCard: View {
    let tree: ScanTree
    let item: Int
    let parentSize: UInt64

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            ItemIcon(tree: tree, item: item, size: 32)
            VStack(alignment: .leading, spacing: 2) {
                Text(tree.name(item))
                    .font(.headline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text("\(Format.bytes(tree.size[item])) · \(Format.percent(tree.size[item], of: parentSize)) of “\(tree.name(Int(tree.parent[item])))”")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                if tree.isDirectory[item] {
                    Text(Format.items(Int(tree.items[item])))
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                } else {
                    Text(tree.location(item))
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .lineLimit(1)
                        .truncationMode(.head)
                }
            }
            .frame(maxWidth: 220, alignment: .leading)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .glassEffect(.regular, in: .rect(cornerRadius: 14))
    }
}
