import QuickLook
import SwiftUI

struct BrowserView: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }
    @State private var inspectorShown = false
    @AppStorage(Preferences.showTreemap) private var showTreemap = true

    var body: some View {
        @Bindable var model = model
        NavigationSplitView {
            SidebarView()
                .navigationSplitViewColumnWidth(min: 210, ideal: 240, max: 320)
        } detail: {
            detail
                .inspector(isPresented: $inspectorShown) {
                    InspectorView()
                        .inspectorColumnWidth(min: 240, ideal: 270, max: 340)
                }
        }
        .navigationTitle(title)
        .navigationSubtitle(subtitle)
        .searchable(text: $model.searchText, placement: .toolbar, prompt: searchPrompt)
        .toolbar { toolbar }
        .quickLookPreview($model.quickLookURL)
    }

    @ViewBuilder
    private var detail: some View {
        switch model.sidebar ?? .folders {
        case .folders:
            if showTreemap && model.searchText.isEmpty {
                VSplitView {
                    TreemapView()
                        .padding(8)
                        .frame(minHeight: 150, idealHeight: 320, maxHeight: .infinity)
                    FolderTable()
                        .frame(minHeight: 160, idealHeight: 300, maxHeight: .infinity)
                }
                .safeAreaInset(edge: .bottom, spacing: 0) { PathBar() }
            } else {
                FolderTable()
                    .safeAreaInset(edge: .bottom, spacing: 0) { PathBar() }
            }
        case .largestFiles:
            FilesTable(category: nil)
        case .category(let category):
            FilesTable(category: category)
        }
    }

    private var title: String {
        switch model.sidebar ?? .folders {
        case .folders: tree.name(browser.directory)
        case .largestFiles: "Largest Files"
        case .category(let category): category.title
        }
    }

    private var subtitle: String {
        if model.isUpdating { return "Updating…" }
        return detailSubtitle
    }

    private var detailSubtitle: String {
        _ = browser.revision
        switch model.sidebar ?? .folders {
        case .folders:
            let d = browser.directory
            return "\(Format.bytes(tree.size[d])) · \(Format.items(Int(tree.items[d])))"
        case .largestFiles:
            return "\(Format.count(tree.fileCount)) files in \(tree.rootURL.lastPathComponent)"
        case .category(let category):
            return Format.bytes(tree.categoryTotals[category] ?? 0)
        }
    }

    private var searchPrompt: String {
        model.sidebar == .folders || model.sidebar == nil ? "Search in \(tree.name(browser.directory))" : "Search files"
    }

    @ToolbarContentBuilder
    private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .navigation) {
            ControlGroup {
                Button("Back", systemImage: "chevron.left") { model.goBack() }
                    .disabled(!model.canGoBack)
                Button("Forward", systemImage: "chevron.right") { model.goForward() }
                    .disabled(!model.canGoForward)
            }
            .controlGroupStyle(.navigation)
            .help("See folders you viewed previously")
        }

        ToolbarItemGroup(placement: .primaryAction) {
            let selected = browser.selectedItems
            Button("Quick Look", systemImage: "eye") { model.quickLook() }
                .disabled(selected.isEmpty)
                .help("Preview the selected item (Space)")
            Button("Show in Finder", systemImage: "finder") { model.reveal(selected) }
                .disabled(selected.isEmpty)
                .help("Show the selected items in Finder")
            Button("Move to Trash", systemImage: "trash") { model.requestDeletion(selected) }
                .disabled(selected.isEmpty)
                .help("Move the selected items to the Trash (⌘⌫)")
        }

        ToolbarSpacer(.fixed, placement: .primaryAction)

        ToolbarItemGroup(placement: .primaryAction) {
            if model.sidebar == .folders || model.sidebar == nil {
                Toggle(isOn: $showTreemap) {
                    Label("Treemap", systemImage: "square.grid.3x3.square")
                }
                .help(showTreemap ? "Hide the treemap" : "Show the treemap")
            }
            if model.isUpdating {
                ProgressView()
                    .controlSize(.small)
                    .help("Checking for changes since the last scan…")
            } else {
                Button("Rescan", systemImage: "arrow.clockwise") { model.rescan() }
                    .help("Update with changes since the last scan (⌘R). Full Rescan: ⌥⌘R")
            }
            Button("Info", systemImage: "info.circle") { inspectorShown.toggle() }
                .help("Show details about the selection (⌥⌘I)")
                .keyboardShortcut("i", modifiers: [.command, .option])
        }
    }
}

// MARK: - Sidebar

struct SidebarView: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }

    var body: some View {
        @Bindable var model = model
        let _ = browser.revision
        List(selection: $model.sidebar) {
            Section(tree.rootURL.lastPathComponent) {
                Label("Folders", systemImage: "folder")
                    .badge(Text(Format.bytes(tree.totalSize)))
                    .tag(SidebarItem.folders)
                Label("Largest Files", systemImage: "list.number")
                    .badge(Text(Format.count(tree.fileCount)))
                    .tag(SidebarItem.largestFiles)
            }
            Section("Kinds") {
                ForEach(kinds, id: \.self) { category in
                    Label {
                        Text(category.title)
                    } icon: {
                        Image(systemName: category.symbol)
                            .foregroundStyle(category.color)
                    }
                    .badge(Text(Format.bytes(tree.categoryTotals[category] ?? 0)))
                    .tag(SidebarItem.category(category))
                }
            }
        }
        .listStyle(.sidebar)
        .safeAreaInset(edge: .bottom, spacing: 0) {
            SummaryCard()
                .padding(10)
        }
    }

    private var kinds: [FileCategory] {
        FileCategory.fileKinds
            .filter { (tree.categoryTotals[$0] ?? 0) > 0 }
            .sorted { (tree.categoryTotals[$0] ?? 0) > (tree.categoryTotals[$1] ?? 0) }
    }
}

struct SummaryCard: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
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
                Label("\(Format.bytes(tree.freedBytes)) freed", systemImage: "checkmark.circle.fill")
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
                    Label("Some folders couldn’t be read", systemImage: "exclamationmark.triangle.fill")
                        .font(.caption.weight(.medium))
                        .foregroundStyle(.orange)
                    Text("Sizes may be larger than shown.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                    Button("Allow Full Disk Access…") { AppModel.openFullDiskAccessSettings() }
                        .buttonStyle(.link)
                        .font(.caption)
                }
                .help(tree.reason)
            }
        }
        .padding(12)
        .glassEffect(.regular, in: .rect(cornerRadius: 14))
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

struct FolderTable: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }
    @State private var rows: [ItemRow] = []
    @State private var sortOrder = [KeyPathComparator(\ItemRow.size, order: .reverse)]

    private struct Key: Equatable { var directory: Int; var revision: Int; var query: String }

    var body: some View {
        @Bindable var browser = browser
        let parentSize = tree.size[browser.directory]
        let searching = !model.searchText.isEmpty
        Table(rows, selection: $browser.selection, sortOrder: $sortOrder) {
            TableColumn("Name", value: \.name) { row in
                HStack(spacing: 7) {
                    ItemIcon(tree: tree, item: row.id)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(row.name)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        if searching {
                            Text(row.location)
                                .font(.caption)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.head)
                        }
                    }
                }
            }
            .width(min: 180, ideal: 320)

            TableColumn("Size", value: \.size) { row in
                Text(Format.bytes(row.size))
                    .monospacedDigit()
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
            .width(min: 70, ideal: 90, max: 120)

            TableColumn("Share") { row in
                HStack(spacing: 8) {
                    SizeBar(fraction: parentSize > 0 ? Double(row.size) / Double(parentSize) : 0,
                            color: row.category.color)
                    Text(Format.percent(row.size, of: parentSize))
                        .font(.caption)
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                        .frame(width: 42, alignment: .trailing)
                }
            }
            .width(min: 120, ideal: 200)

            TableColumn("Items", value: \.items) { row in
                Text(row.isDirectory ? Format.count(row.items) : "—")
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
            .width(min: 50, ideal: 70, max: 100)
        }
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
                    ContentUnavailableView("Empty Folder", systemImage: "folder",
                                           description: Text("Nothing in this folder takes up space."))
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

struct FilesTable: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }
    let category: FileCategory?
    @State private var rows: [ItemRow] = []
    @State private var sortOrder = [KeyPathComparator(\ItemRow.size, order: .reverse)]

    private struct Key: Equatable { var category: FileCategory?; var revision: Int; var query: String }
    static let limit = 1000

    var body: some View {
        @Bindable var browser = browser
        Table(rows, selection: $browser.selection, sortOrder: $sortOrder) {
            TableColumn("Name", value: \.name) { row in
                HStack(spacing: 7) {
                    ItemIcon(tree: tree, item: row.id)
                    Text(row.name)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            .width(min: 180, ideal: 300)

            TableColumn("Size", value: \.size) { row in
                Text(Format.bytes(row.size))
                    .monospacedDigit()
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
            .width(min: 70, ideal: 90, max: 120)

            TableColumn("Share of Scan") { row in
                SizeBar(fraction: Double(row.size) / Double(max(largest, 1)), color: row.category.color)
                    .help(Format.percent(row.size, of: tree.totalSize) + " of everything scanned")
            }
            .width(min: 80, ideal: 140)

            TableColumn("Where", value: \.location) { row in
                Text(row.location)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.head)
            }
            .width(min: 120, ideal: 280)
        }
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
                    ContentUnavailableView("No Files", systemImage: category?.symbol ?? "doc",
                                           description: Text("There are no files of this kind in the scan."))
                } else {
                    ContentUnavailableView.search(text: model.searchText)
                }
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            if tree.fileCount > Self.limit || category != nil {
                HStack {
                    Text(footer)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                .padding(.horizontal, 14)
                .padding(.vertical, 6)
                .background(.bar)
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

// MARK: - Path bar

struct PathBar: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }

    var body: some View {
        let chain = tree.lineage(browser.directory)
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 2) {
                ForEach(Array(chain.enumerated()), id: \.element) { index, item in
                    if index > 0 {
                        Image(systemName: "chevron.compact.right")
                            .foregroundStyle(.tertiary)
                    }
                    Button {
                        model.navigate(to: item)
                    } label: {
                        HStack(spacing: 4) {
                            ItemIcon(tree: tree, item: item, size: 14)
                            Text(tree.name(item))
                        }
                        .padding(.horizontal, 4)
                        .padding(.vertical, 2)
                        .contentShape(.rect)
                    }
                    .buttonStyle(.plain)
                    .fontWeight(item == browser.directory ? .medium : .regular)
                }
            }
            .font(.caption)
            .padding(.horizontal, 12)
            .padding(.vertical, 5)
        }
        .defaultScrollAnchor(.trailing)
        .background(.bar)
        .overlay(alignment: .top) { Divider() }
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
                    Image(systemName: "square.stack.3d.up.fill")
                        .font(.system(size: 44))
                        .foregroundStyle(.secondary)
                    Text("\(items.count) Items").font(.title3.weight(.semibold))
                    Text(Format.bytes(bytes)).font(.title2).monospacedDigit()
                    actions(items)
                }
                .padding()
                .frame(maxWidth: .infinity)
            } else {
                detail(model.sidebar == .folders || model.sidebar == nil ? browser.directory : tree.root)
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
                Label("Show in Finder", systemImage: "finder").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            Button(role: .destructive) { model.requestDeletion(items) } label: {
                Label("Move to Trash", systemImage: "trash").frame(maxWidth: .infinity)
            }
            .buttonStyle(.glass)
            .tint(.red)
        }
        .controlSize(.large)
    }
}
