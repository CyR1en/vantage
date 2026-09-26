import QuickLook
import SwiftUI

struct BrowserView: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }
    @AppStorage(Preferences.showTreemap) private var showTreemap = true

    var body: some View {
        @Bindable var model = model
        detail
            .navigationTitle(title)
            .quickLookPreview($model.quickLookURL)
    }

    @ViewBuilder
    private var detail: some View {
        switch model.sidebar {
        case .folders:
            if showTreemap && model.searchText.isEmpty {
                TreemapSplit()
            } else {
                FolderList()
            }
        case .largestFiles:
            FileList(category: nil)
        case .category(let category):
            FileList(category: category)
        }
    }

    private var title: String {
        switch model.sidebar {
        case .folders: tree.name(browser.directory)
        case .largestFiles: "Largest Files"
        case .category(let category): category.title
        }
    }
}

/// The treemap above the folder list, split by a draggable handle.
/// (VSplitView's AppKit panes misplace their content inside the inset card.)
struct TreemapSplit: View {
    @AppStorage(Preferences.treemapFraction) private var fraction = 0.5
    @State private var dragStart: CGFloat?
    @State private var dragFraction: Double?   // live value; persisted when the drag ends
    private static let minTreemap: CGFloat = 150
    private static let minList: CGFloat = 160

    var body: some View {
        GeometryReader { proxy in
            let total = proxy.size.height
            let top = clamp(total * (dragFraction ?? fraction), total)
            VStack(spacing: 0) {
                TreemapView()
                    .padding(8)
                    .frame(height: top)
                SplitHandle()
                    .gesture(
                        // Global space: the handle moves with the drag, so local translations would feed back.
                        DragGesture(minimumDistance: 1, coordinateSpace: .global)
                            .onChanged { value in
                                let start = dragStart ?? top
                                dragStart = start
                                dragFraction = clamp(start + value.translation.height, total) / max(total, 1)
                            }
                            .onEnded { _ in
                                if let dragFraction { fraction = dragFraction }
                                dragFraction = nil
                                dragStart = nil
                            }
                    )
                FolderList()
                    .frame(maxHeight: .infinity)
            }
        }
    }

    private func clamp(_ height: CGFloat, _ total: CGFloat) -> CGFloat {
        min(max(height, Self.minTreemap), max(total - Self.minList, Self.minTreemap))
    }
}

private struct SplitHandle: View {
    @State private var hovering = false

    var body: some View {
        ZStack {
            Rectangle()
                .fill(Color.primary.opacity(0.08))
                .frame(height: 1)
            Capsule()
                .fill(Color.primary.opacity(hovering ? 0.35 : 0.18))
                .frame(width: hovering ? 44 : 36, height: 4)
        }
        .frame(height: 10)
        .frame(maxWidth: .infinity)
        .contentShape(.rect)
        .pointerStyle(.frameResize(position: .top))
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.15), value: hovering)
        .accessibilityHidden(true)
    }
}

// MARK: - Summary

struct SummaryCard: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    @Environment(\.openWindow) private var openWindow
    private var tree: ScanTree { browser.tree }

    var body: some View {
        let _ = browser.revision
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(Format.bytes(tree.totalSize))
                    .font(.title3.weight(.semibold))
                    .monospacedDigit()
                Spacer()
                Text(tree.allocated ? "used on disk" : "apparent size")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            CategoryStrip(tree: tree)
            Text("\(Format.count(tree.fileCount)) files · \(Format.count(tree.folderCount)) folders")
                .font(.caption)
                .foregroundStyle(.secondary)
            if tree.freedBytes > 0 {
                Label("\(Format.bytes(tree.freedBytes)) freed", icon: .done, weight: .fill, size: 13)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.green)
            }
            if let checked = model.lastChecked {
                Text("Updated \(checked, format: .relative(presentation: .named))")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            if tree.isPartial {
                VStack(alignment: .leading, spacing: 4) {
                    Label("Some folders couldn’t be read", icon: .warning, weight: .fill, size: 13)
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.orange)
                    Text("Sizes may be larger than shown.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    if AccessGuide.shared.status != .granted {
                        Button("Turn On Full Disk Access…") { AccessGuide.shared.show(openWindow) }
                            .buttonStyle(.link)
                            .font(.caption)
                    }
                }
                .help(tree.reason)
            }
        }
        .padding(16)
    }
}

/// Storage-settings–style strip showing each kind's share of the scan.
struct CategoryStrip: View {
    let tree: ScanTree

    var body: some View {
        let total = max(tree.totalSize, 1)
        let parts = FileCategory.fileKinds
            .map { ($0, tree.categoryTotals[$0] ?? 0) }
            .filter { $0.1 > 0 }
            .sorted { $0.1 > $1.1 }
        GeometryReader { proxy in
            HStack(spacing: 1.5) {
                ForEach(parts, id: \.0) { category, bytes in
                    Rectangle()
                        .fill(category.color.gradient)
                        .frame(width: max(2, proxy.size.width * CGFloat(Double(bytes) / Double(total)) - 1.5))
                        .help("\(category.title): \(Format.bytes(bytes))")
                }
                Spacer(minLength: 0)
            }
        }
        .frame(height: 8)
        .background(.quaternary)
        .clipShape(Capsule())
    }
}

// MARK: - Folder list

struct FolderList: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }
    @State private var rows: [ItemRow] = []
    @State private var sortOrder: RowSort = [KeyPathComparator(\ItemRow.size, order: .reverse)]
    @State private var compact = false

    private struct Key: Equatable { var directory: Int; var revision: Int; var query: String }

    var body: some View {
        @Bindable var browser = browser
        let parentSize = tree.size[browser.directory]
        let searching = !model.searchText.isEmpty
        List(rows, selection: $browser.selection) { row in
            FolderRowView(tree: tree, row: row, parentSize: parentSize, searching: searching, compact: compact)
                .cardListRow()
        }
        .cardList()
        .safeAreaBar(edge: .top) {
            CardListHeader {
                SortButton(title: "Name", key: \ItemRow.name, order: $sortOrder)
                if !compact {
                    Text("Share")
                        .foregroundStyle(.secondary)
                        .frame(width: ShareCell.width, alignment: .leading)
                    SortButton(title: "Items", key: \ItemRow.items, descendingFirst: true, alignment: .trailing, order: $sortOrder)
                        .frame(width: CardList.itemsWidth)
                }
                SortButton(title: "Size", key: \ItemRow.size, descendingFirst: true, alignment: .trailing, order: $sortOrder)
                    .frame(width: CardList.sizeWidth)
            }
        }
        .onGeometryChange(for: Bool.self) { $0.size.width < CardList.compactWidth } action: { compact = $0 }
        .contextMenu(forSelectionType: Int.self) { ids in
            ItemMenu(items: Array(ids).sorted())
        } primaryAction: { ids in
            if ids.count == 1, let id = ids.first { model.open(id) }
        }
        .onKeyPress(.space) {
            model.quickLookURL == nil ? model.quickLook() : (model.quickLookURL = nil)
            return .handled
        }
        .overlay {
            if rows.isEmpty {
                if searching {
                    ContentUnavailableView.search(text: model.searchText)
                } else {
                    ContentUnavailableView {
                        Label("Empty Folder", icon: .folders, weight: .duotone, size: 48)
                    } description: {
                        Text("Nothing in this folder takes up space.")
                    }
                }
            }
        }
        .task(id: Key(directory: browser.directory, revision: browser.revision, query: model.searchText)) {
            if searching { try? await Task.sleep(for: .milliseconds(200)) }
            guard !Task.isCancelled else { return }
            await rebuild()
        }
        .onChange(of: sortOrder) { rows.sort(using: sortOrder) }
    }

    private func rebuild() async {
        let snapshot = tree.snapshot(), directory = browser.directory, query = model.searchText
        guard let built = try? await BackgroundWork.run({
            snapshot.folderRows(directory: directory, query: query)
        }), !Task.isCancelled else { return }
        rows = built.sorted(using: sortOrder)
    }
}

// MARK: - Largest files

struct FileList: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }
    let category: FileCategory?
    @State private var rows: [ItemRow] = []
    @State private var sortOrder: RowSort = [KeyPathComparator(\ItemRow.size, order: .reverse)]
    @State private var compact = false

    private struct Key: Equatable { var category: FileCategory?; var revision: Int; var query: String }
    static let limit = 1000

    var body: some View {
        @Bindable var browser = browser
        let largest = largest
        List(rows, selection: $browser.selection) { row in
            FileRowView(tree: tree, row: row, largest: largest, compact: compact)
                .cardListRow()
        }
        .cardList()
        .safeAreaBar(edge: .top) {
            CardListHeader {
                HStack(spacing: 14) {
                    SortButton(title: "Name", key: \ItemRow.name, fills: false, order: $sortOrder)
                    SortButton(title: "Location", key: \ItemRow.location, fills: false, order: $sortOrder)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                if !compact {
                    Text("Share of Scan")
                        .foregroundStyle(.secondary)
                        .frame(width: ShareCell.width, alignment: .leading)
                }
                SortButton(title: "Size", key: \ItemRow.size, descendingFirst: true, alignment: .trailing, order: $sortOrder)
                    .frame(width: CardList.sizeWidth)
            }
        }
        .onGeometryChange(for: Bool.self) { $0.size.width < CardList.compactWidth } action: { compact = $0 }
        .contextMenu(forSelectionType: Int.self) { ids in
            ItemMenu(items: Array(ids).sorted())
        } primaryAction: { ids in
            if ids.count == 1, let id = ids.first { model.quickLook(id) }
        }
        .onKeyPress(.space) {
            model.quickLookURL == nil ? model.quickLook() : (model.quickLookURL = nil)
            return .handled
        }
        .overlay {
            if rows.isEmpty {
                if model.searchText.isEmpty {
                    ContentUnavailableView {
                        Label("No Files", icon: category?.icon ?? .documents, weight: .duotone, size: 48)
                    } description: {
                        Text("There are no files of this kind in the scan.")
                    }
                } else {
                    ContentUnavailableView.search(text: model.searchText)
                }
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            if tree.fileCount > Self.limit || category != nil {
                Text(footer)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 6)
                    .glassEffect(.regular, in: .capsule)
                    .padding(.bottom, 10)
            }
        }
        .task(id: Key(category: category, revision: browser.revision, query: model.searchText)) {
            if !model.searchText.isEmpty { try? await Task.sleep(for: .milliseconds(200)) }
            guard !Task.isCancelled else { return }
            let snapshot = tree.snapshot(), query = model.searchText, limit = Self.limit, filter = category
            guard let built = try? await BackgroundWork.run({
                snapshot.fileRows(category: filter, query: query, limit: limit)
            }), !Task.isCancelled else { return }
            rows = built.sorted(using: sortOrder)
        }
        .onChange(of: sortOrder) { rows.sort(using: sortOrder) }
    }

    private var largest: UInt64 { rows.map(\.size).max() ?? 1 }

    private var footer: String {
        let shown = rows.count
        if let category {
            return "Showing the \(Format.count(shown)) largest \(category.title.lowercased()) · \(Format.bytes(tree.categoryTotals[category] ?? 0)) total"
        }
        return "Showing the \(Format.count(shown)) largest of \(Format.count(tree.fileCount)) files"
    }
}

// MARK: - Inspector

struct InspectorView: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }

    var body: some View {
        let _ = browser.revision
        let items = browser.selectedItems
        ScrollView {
            if items.count == 1 {
                detail(items[0])
            } else if items.count > 1 {
                let bytes = items.reduce(UInt64(0)) { $0 + tree.size[$1] }
                VStack(spacing: 12) {
                    IconImage(.selection, weight: .duotone, size: 52)
                        .foregroundStyle(.secondary)
                    Text("\(items.count) Items").font(.title3.weight(.semibold))
                    Text(Format.bytes(bytes)).font(.title2).monospacedDigit()
                    actions(items)
                }
                .padding()
                .frame(maxWidth: .infinity)
            } else {
                detail(model.sidebar == .folders ? browser.directory : tree.root)
            }
        }
    }

    @ViewBuilder
    private func detail(_ item: Int) -> some View {
        let url = tree.url(item)
        let parent = item == tree.root ? item : Int(tree.parent[item])
        VStack(alignment: .leading, spacing: 14) {
            VStack(spacing: 8) {
                ItemIcon(tree: tree, item: item, size: 72)
                Text(tree.name(item))
                    .font(.headline)
                    .multilineTextAlignment(.center)
                    .textSelection(.enabled)
                Text(Format.bytes(tree.size[item]))
                    .font(.title2.weight(.semibold))
                    .monospacedDigit()
            }
            .frame(maxWidth: .infinity)

            Form {
                LabeledContent("Kind", value: tree.isDirectory[item] ? "Folder" : tree.category[item].title)
                if tree.isDirectory[item] {
                    LabeledContent("Contains", value: Format.items(Int(tree.items[item])))
                }
                if item != tree.root {
                    LabeledContent("Share of folder", value: Format.percent(tree.size[item], of: tree.size[parent]))
                }
                LabeledContent("Share of scan", value: Format.percent(tree.size[item], of: tree.totalSize))
                LabeledContent("Where") {
                    Text((url.deletingLastPathComponent().path as NSString).abbreviatingWithTildeInPath)
                        .textSelection(.enabled)
                        .multilineTextAlignment(.trailing)
                }
            }
            .formStyle(.columns)
            .font(.callout)

            if item != tree.root { actions([item]) }
        }
        .padding()
    }

    private func actions(_ items: [Int]) -> some View {
        VStack(spacing: 8) {
            Button { model.reveal(items) } label: {
                Label("Show in Finder", icon: .reveal).frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            Button(role: .destructive) { model.requestDeletion(items) } label: {
                Label("Move to Trash", icon: .trash).frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            .tint(.red)
        }
        .controlSize(.large)
    }
}
