import AppKit
import Observation
import SwiftUI

enum SidebarItem: Hashable {
    case folders
    case largestFiles
    case category(FileCategory)
}

@MainActor
@Observable
final class AppModel {
    enum Phase {
        case welcome
        case scanning(URL)
        case browsing
        case failed(URL, String)
    }

    var phase: Phase = .welcome
    private(set) var browser: BrowserState?
    var tree: ScanTree? { browser?.tree }
    var scanProgress = Scanner.Progress(entries: 0, elapsed: 0)

    var sidebar: SidebarItem = .folders {
        didSet { if sidebar != oldValue { selection = []; searchText = "" } }
    }
    var directory: Int {
        get { browser?.directory ?? 0 }
        set { browser?.directory = newValue }
    }
    var selection: Set<Int> {
        get { browser?.selection ?? [] }
        set { browser?.selection = newValue }
    }
    var searchText = ""
    /// The location bar shows a search field instead of the path.
    var isSearching = false
    var inspectorShown = false
    /// The rail shows labels; remembered for new windows.
    var railExpanded = UserDefaults.standard.bool(forKey: Preferences.railExpanded) {
        didSet { UserDefaults.standard.set(railExpanded, forKey: Preferences.railExpanded) }
    }
    /// The expanded rail's width, dragged from its edge; saved when a drag ends.
    var railExpandedWidth: CGFloat = {
        let saved = UserDefaults.standard.double(forKey: Preferences.railWidth)
        return saved > 0 ? min(max(saved, Chrome.railMinWidth), Chrome.railMaxWidth) : Chrome.railExpandedWidth
    }()
    var quickLookURL: URL?
    var revision: Int {
        get { browser?.revision ?? 0 }
        set { browser?.revision = newValue }
    }

    var pendingDeletion: DeletionRequest?
    var alert: AlertMessage?
    var toast: String?

    /// The saved-scan record for the current tree (its change-journal mark and date).
    private(set) var record: ScanCache.Record?
    /// True while an incremental refresh runs; deletions wait until it finishes.
    private(set) var isUpdating = false
    private(set) var lastChecked: Date?
    /// True while a saved scan is being loaded (as opposed to a fresh scan running).
    private(set) var isOpeningSaved = false

    private var backStack: [Int] {
        get { browser?.backStack ?? [] }
        set { browser?.backStack = newValue }
    }
    private var forwardStack: [Int] {
        get { browser?.forwardStack ?? [] }
        set { browser?.forwardStack = newValue }
    }
    private var scanner: Scanner?
    private var workTask: Task<Void, Never>?
    private var generation = 0
    private var toastTask: Task<Void, Never>?

    struct DeletionRequest: Identifiable {
        let id = UUID()
        let treeID: UUID
        let items: [Int]
        let permanently: Bool
        let bytes: UInt64
        let title: String
    }

    struct AlertMessage: Identifiable {
        let id = UUID()
        let title: String
        let message: String
    }

    var scannedURL: URL? {
        switch phase {
        case .scanning(let url), .failed(let url, _): url
        case .browsing: tree?.rootURL
        case .welcome: nil
        }
    }

    var isBrowsing: Bool { if case .browsing = phase { true } else { false } }

    // MARK: Scanning

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Scan"
        panel.message = "Choose a folder or disk to see what’s taking up space."
        panel.directoryURL = tree?.rootURL ?? FileManager.default.homeDirectoryForCurrentUser
        if panel.runModal() == .OK, let url = panel.url { scan(url) }
    }

    /// Opens a folder: a saved scan appears immediately and is then brought up
    /// to date incrementally; otherwise a full scan runs.
    func scan(_ url: URL) {
        let allocated = measuresDiskUsage
        guard let saved = ScanCache.record(for: url, allocated: allocated) else {
            fullScan(url)
            return
        }
        let generation = beginWork()
        isOpeningSaved = true
        phase = .scanning(url)
        scanProgress = .init(entries: 0, elapsed: 0)
        workTask = Task {
            defer { if generation == self.generation { self.isOpeningSaved = false } }
            do {
                let tree = try await BackgroundWork.run { try ScanCache.load(saved) }
                guard generation == self.generation else { return }
                self.show(tree, record: saved, keepPlace: false)
                self.refresh()
            } catch {
                guard generation == self.generation else { return }
                try? await BackgroundWork.run(priority: .utility) { ScanCache.forget(saved) }
                guard generation == self.generation else { return }
                self.fullScan(url)
            }
        }
    }

    /// Scans everything from scratch, ignoring any saved scan.
    func fullScan(_ url: URL? = nil) {
        guard let url = url ?? scannedURL else { return }
        let generation = beginWork()
        let scanner = Scanner()
        self.scanner = scanner
        let keepPlace = tree?.rootURL.path == ScanCache.canonical(url)
        scanProgress = .init(entries: 0, elapsed: 0)
        phase = .scanning(url)
        let allocated = measuresDiskUsage
        let mark = ChangeJournal.mark(for: url)   // before scanning, so nothing is missed
        workTask = Task {
            do {
                let (tree, data) = try await scanner.scanExport(url, allocated: allocated) { progress in
                    Task { @MainActor in
                        guard generation == self.generation else { return }
                        self.scanProgress = progress
                    }
                }
                guard generation == self.generation else { return }
                let record = ScanCache.Record(root: tree.rootURL.path, allocated: tree.allocated, mark: mark,
                                              scannedAt: Date(), totalSize: tree.totalSize, fileCount: tree.fileCount)
                self.show(tree, record: record, keepPlace: keepPlace)
                NSDocumentController.shared.noteNewRecentDocumentURL(url)
                Task.detached(priority: .utility) { ScanCache.save(data, record: record) }
            } catch Scanner.Failure.cancelled {
                guard generation == self.generation else { return }
                self.phase = self.tree == nil ? .welcome : .browsing
            } catch is CancellationError {
                guard generation == self.generation else { return }
                self.phase = self.tree == nil ? .welcome : .browsing
            } catch {
                guard generation == self.generation else { return }
                self.phase = .failed(url, error.localizedDescription)
            }
            if self.scanner === scanner { self.scanner = nil }
        }
    }

    /// Re-reads only the folders that changed since the tree was last brought up to date.
    func refresh() {
        guard isBrowsing, let tree, let record, !isUpdating else {
            if tree == nil { rescanFailedOrCancelled() }
            return
        }
        let generation = self.generation
        isUpdating = true
        pendingDeletion = nil
        let started = Date()
        let newMark = ChangeJournal.mark(for: tree.rootURL)
        let snapshot = tree.snapshot()
        workTask = Task {
            defer { if generation == self.generation { self.isUpdating = false } }
            let changes = await ChangeJournal.changes(under: tree.rootURL, since: record.mark)
            guard generation == self.generation else { return }
            if !tree.isPartial && !changes.needsFullScan && changes.shallow.isEmpty && changes.deep.isEmpty {
                self.lastChecked = Date()
                self.showToast("Up to date")
                return
            }
            let result: IncrementalScan.Result?
            do {
                result = try await BackgroundWork.run {
                    try await snapshot.refresh(changes: changes, started: started)
                }
            } catch {
                result = nil
            }
            guard generation == self.generation else { return }
            guard let result else {
                self.isUpdating = false
                self.fullScan(tree.rootURL)
                return
            }
            result.tree.carryFreedBytes(from: tree)
            let updated = ScanCache.Record(root: record.root, allocated: record.allocated, mark: newMark,
                                           scannedAt: Date(), totalSize: result.tree.totalSize,
                                           fileCount: result.tree.fileCount)
            self.show(result.tree, record: updated, keepPlace: true)
            self.showToast("Updated in \(Format.duration(Date().timeIntervalSince(started)))")
            let fresh = result.tree.snapshot()
            try? await BackgroundWork.run(priority: .utility) {
                let data = fresh.serialize()
                try Task.checkCancellation()
                ScanCache.save(data, record: updated)
            }
        }
    }

    private func rescanFailedOrCancelled() {
        if let url = scannedURL { scan(url) }
    }

    func cancelScan() {
        _ = beginWork()
        phase = tree == nil ? .welcome : .browsing
    }

    func rescan() {
        if isBrowsing { refresh() } else if let url = scannedURL { scan(url) }
    }

    func closeScan() {
        _ = beginWork()
        browser = nil
        record = nil
        isUpdating = false
        selection = []
        searchText = ""
        isSearching = false
        phase = .welcome
    }

    private var measuresDiskUsage: Bool {
        // Actual disk usage by default: sparse files such as VM disk images (OrbStack,
        // Docker, UTM) report a huge apparent size but occupy only their allocated blocks.
        !UserDefaults.standard.bool(forKey: Preferences.apparentSize)
    }

    private func beginWork() -> Int {
        generation += 1
        workTask?.cancel()
        workTask = nil
        pendingDeletion = nil
        isOpeningSaved = false
        scanner?.cancel()
        scanner = nil
        isUpdating = false
        return generation
    }

    /// Installs a tree, keeping the current folder and selection when they still exist.
    func show(_ newTree: ScanTree, record: ScanCache.Record, keepPlace: Bool) {
        let old = tree
        let next = BrowserState(tree: newTree)
        pendingDeletion = nil
        quickLookURL = nil
        self.record = record
        lastChecked = record.scannedAt
        func remap(_ i: Int) -> Int? {
            guard let old, i >= 0, i <= old.count, !old.isRemoved(i) else { return nil }
            return newTree.find(relativePath: old.relativeComponents(i))
        }
        if keepPlace, let old {
            var place = old.lineage(directory)
            var mapped: Int?
            while mapped == nil, let last = place.popLast() {
                if let candidate = remap(last), newTree.isDirectory[candidate] { mapped = candidate }
            }
            next.directory = mapped ?? newTree.root
            next.selection = Set(selection.compactMap(remap))
            next.backStack = backStack.compactMap(remap).filter { newTree.isDirectory[$0] }
            next.forwardStack = forwardStack.compactMap(remap).filter { newTree.isDirectory[$0] }
        } else {
            searchText = ""
            sidebar = .folders
        }
        browser = next
        phase = .browsing
    }

    // MARK: Navigation

    var canGoBack: Bool { !backStack.isEmpty }
    var canGoForward: Bool { !forwardStack.isEmpty }
    var canGoUp: Bool { tree.map { directory != $0.root } ?? false }

    func open(_ item: Int) {
        guard let tree else { return }
        if tree.isDirectory[item] {
            navigate(to: item)
        } else {
            quickLook(item)
        }
    }

    func navigate(to item: Int, select child: Int? = nil) {
        guard let tree, tree.isDirectory[item], !tree.isRemoved(item) else { return }
        if sidebar != .folders { sidebar = .folders }
        if item != directory {
            backStack.append(directory)
            forwardStack.removeAll()
            directory = item
        }
        searchText = ""
        selection = child.map { [$0] } ?? []
    }

    func showInFolder(_ item: Int) {
        guard let tree else { return }
        navigate(to: Int(tree.parent[item]), select: item)
    }

    func goBack() {
        guard let previous = backStack.popLast() else { return }
        forwardStack.append(directory)
        let from = directory
        directory = previous
        sidebar = .folders
        selection = tree?.parent[from] == Int32(previous) ? [from] : []
    }

    func goForward() {
        guard let next = forwardStack.popLast() else { return }
        backStack.append(directory)
        directory = next
        sidebar = .folders
        selection = []
    }

    func goUp() {
        guard let tree, directory != tree.root else { return }
        let from = directory
        navigate(to: Int(tree.parent[directory]), select: from)
    }

    func goToRoot() {
        guard let tree else { return }
        navigate(to: tree.root)
    }

    // MARK: Item actions

    var selectedItems: [Int] {
        browser?.selectedItems ?? []
    }

    func quickLook(_ item: Int? = nil) {
        guard let tree, let target = item ?? selectedItems.first else { return }
        quickLookURL = tree.url(target)
    }

    func reveal(_ items: [Int]) {
        guard let tree, !items.isEmpty else { return }
        NSWorkspace.shared.activateFileViewerSelecting(items.map(tree.url))
    }

    func openWithDefaultApp(_ items: [Int]) {
        guard let tree else { return }
        for item in items { NSWorkspace.shared.open(tree.url(item)) }
    }

    func copyPaths(_ items: [Int]) {
        guard let tree, !items.isEmpty else { return }
        let board = NSPasteboard.general
        board.clearContents()
        board.setString(items.map { tree.url($0).path }.joined(separator: "\n"), forType: .string)
        showToast(items.count == 1 ? "Path copied" : "\(items.count) paths copied")
    }

    func requestDeletion(_ items: [Int], permanently: Bool = false) {
        guard isBrowsing, let tree else { return }
        if isUpdating {
            showToast("Finishing the update first — try again in a moment")
            return
        }
        // Drop items already covered by a selected ancestor.
        let set = Set(items.filter { $0 >= 0 && $0 < tree.count && !tree.isRemoved($0) })
        let roots = set.sorted().filter { item in
            !tree.lineage(item).dropLast().contains(where: set.contains)
        }
        guard !roots.isEmpty else { return }
        let bytes = roots.reduce(UInt64(0)) { $0 + tree.size[$1] }
        let title = roots.count == 1 ? "“\(tree.name(roots[0]))”" : "\(roots.count) items"
        let request = DeletionRequest(treeID: tree.id, items: roots,
                                      permanently: permanently, bytes: bytes, title: title)
        if !permanently && !UserDefaults.standard.bool(forKey: Preferences.confirmTrash, default: true) {
            performDeletion(request)
        } else {
            pendingDeletion = request
        }
    }

    func performDeletion(_ request: DeletionRequest) {
        pendingDeletion = nil
        guard isBrowsing, !isUpdating, let tree, tree.id == request.treeID else {
            showToast("The scan changed. Select the items again before removing them.")
            return
        }
        var freed: UInt64 = 0
        var failures: [(String, String)] = []
        for item in request.items {
            guard item >= 0, item < tree.count, !tree.isRemoved(item) else { continue }
            let url = tree.url(item)
            do {
                if request.permanently {
                    try FileManager.default.removeItem(at: url)
                } else {
                    try FileManager.default.trashItem(at: url, resultingItemURL: nil)
                }
                freed += tree.size[item]
                tree.remove(item)
            } catch {
                failures.append((url.lastPathComponent, (error as NSError).localizedDescription))
            }
        }
        selection.subtract(request.items)
        if tree.isRemoved(directory) {
            directory = tree.lineage(directory).last(where: { !tree.isRemoved($0) }) ?? tree.root
        }
        backStack.removeAll { tree.isRemoved($0) }
        forwardStack.removeAll { tree.isRemoved($0) }
        revision += 1
        if freed > 0 || failures.count < request.items.count {
            NSSound(named: request.permanently ? "Funk" : "Pop")?.play()
            let verb = request.permanently ? "deleted" : "moved to the Trash"
            showToast("\(Format.bytes(freed)) \(verb)")
        }
        if !failures.isEmpty {
            let detail = failures.prefix(5).map { "\($0.0): \($0.1)" }.joined(separator: "\n")
            alert = AlertMessage(
                title: failures.count == 1 ? "An item couldn’t be removed" : "\(failures.count) items couldn’t be removed",
                message: detail)
        }
    }

    func showToast(_ text: String) {
        toastTask?.cancel()
        withAnimation(.snappy) { toast = text }
        toastTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(3))
            guard !Task.isCancelled else { return }
            withAnimation(.smooth) { self?.toast = nil }
        }
    }
}

enum Preferences {
    static let apparentSize = "measureApparentSize"
    static let confirmTrash = "confirmTrash"
    static let showTreemap = "showTreemap"
    static let treemapFraction = "treemapFraction"
    static let railExpanded = "railExpanded"
    static let railWidth = "railWidth"
    static let accessGuide = "fullDiskAccessGuide"
}

extension UserDefaults {
    func bool(forKey key: String, default value: Bool) -> Bool {
        object(forKey: key) == nil ? value : bool(forKey: key)
    }
}
