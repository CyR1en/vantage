import Foundation

/// Brings a saved tree up to date using the folders FSEvents reported as changed.
/// Unchanged folders are copied from the saved tree; changed folders are re-listed;
/// new folders (and folders whose events were coalesced) are scanned by the helper.
enum IncrementalScan {
    struct Result {
        var tree: ScanTree
        var foldersListed: Int
        var subtreesScanned: Int
    }

    /// Returns nil when the changes are too broad and a full scan is the better choice.
    static func refresh(_ old: ScanTree, changes: ChangeJournal.Changes,
                        started: Date = Date()) async throws -> Result? {
        try Task.checkCancellation()
        // Missing entries in a partial scan may never produce a new event.
        if old.isPartial { return nil }
        let rootPath = old.rootURL.path
        let shallow = Set(changes.shallow.compactMap { inScope($0, root: rootPath) })
        let deep = Set(changes.deep.compactMap { inScope($0, root: rootPath) })
        if changes.needsFullScan || deep.contains(rootPath) || shallow.count + deep.count > 50_000 {
            return nil
        }

        var b = TreeBuilder()
        b.reserve(old.count + 1024)
        var pending: [(parent: Int32, url: URL)] = []
        var partial = old.isPartial
        var listed = 0

        var stack: [(old: Int, new: Int32, path: String)] = [(old.root, -1, rootPath)]
        func childPath(_ parent: String, _ name: String) -> String {
            parent == "/" ? "/" + name : parent + "/" + name
        }
        func enqueue(_ oldChild: Int?, _ index: Int32, _ path: String) {
            if deep.contains(path) || oldChild == nil {
                pending.append((index, URL(fileURLWithPath: path, isDirectory: true)))
            } else {
                stack.append((oldChild!, index, path))
            }
        }

        while let (d, np, path) = stack.popLast() {
            try Task.checkCancellation()
            if shallow.contains(path) {
                switch list(path, allocated: old.allocated) {
                case .entries(let entries):
                    listed += 1
                    var previous: [ArraySlice<UInt8>: Int] = [:]
                    for c in old.children(of: d) where old.isDirectory[c] { previous[old.nameBytes(c)] = c }
                    for e in entries {
                        let ni = b.append(parent: np, kind: e.kind, size: e.size, name: e.name)
                        if e.kind == TreeBuilder.directory {
                            enqueue(previous[e.name[...]], ni, childPath(path, String(decoding: e.name, as: UTF8.self)))
                        }
                    }
                    continue
                case .vanished:
                    if d == old.root { throw CocoaError(.fileNoSuchFile) }
                    return nil
                case .unreadable:
                    partial = true   // keep what the saved scan knew
                }
            }
            for c in old.children(of: d) {
                let ni = b.append(parent: np, kind: old.kind[c], size: old.size[c], name: old.nameBytes(c))
                if old.isDirectory[c] { enqueue(c, ni, childPath(path, old.name(c))) }
            }
        }

        if pending.count > 2_000 { return nil }
        let subtrees = try await scanSubtrees(pending.map(\.url), allocated: old.allocated)
        for (entry, subtree) in zip(pending, subtrees) {
            try Task.checkCancellation()
            guard let subtree else { partial = true; continue }
            if subtree.isPartial { partial = true }
            subtree.graft(into: &b, under: entry.parent)
        }

        var info = old.info
        info.completion = partial ? .partial : .complete
        info.scanDuration = Date().timeIntervalSince(started)
        info.method = "incremental"
        let tree = try ScanTree(info: info, builder: b)
        return Result(tree: tree, foldersListed: listed, subtreesScanned: pending.count)
    }

    /// Maps an FSEvents path into the scanned root's namespace (handling the
    /// /System/Volumes/Data firmlink), or nil when it is outside the scan.
    static func inScope(_ path: String, root: String) -> String? {
        let data = "/System/Volumes/Data"
        for candidate in [path, data + path, path.hasPrefix(data + "/") ? String(path.dropFirst(data.count)) : path] {
            if candidate == root || candidate.hasPrefix(root == "/" ? "/" : root + "/") { return candidate }
        }
        return nil
    }

    // MARK: Listing one folder

    struct Entry {
        var name: [UInt8]
        var kind: UInt8
        var size: UInt64
    }

    enum Listing {
        case entries([Entry])
        case vanished
        case unreadable
    }

    private static let keys: Set<URLResourceKey> = [
        .isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey, .isVolumeKey,
        .fileSizeKey, .totalFileAllocatedSizeKey,
    ]

    static func list(_ path: String, allocated: Bool) -> Listing {
        var st = stat()
        guard lstat(path, &st) == 0 else { return errno == ENOENT ? .vanished : .unreadable }
        guard st.st_mode & S_IFMT == S_IFDIR else { return .vanished }
        let url = URL(fileURLWithPath: path, isDirectory: true)
        let contents: [URL]
        do {
            contents = try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: Array(keys), options: [])
        } catch {
            let code = (error as NSError).code
            return code == NSFileReadNoSuchFileError || code == NSFileNoSuchFileError ? .vanished : .unreadable
        }
        var entries: [Entry] = []
        entries.reserveCapacity(contents.count)
        for item in contents {
            if Task.isCancelled { return .unreadable }
            guard let values = try? item.resourceValues(forKeys: keys) else { return .unreadable }
            if values.isVolume == true { continue }       // scans stay on one filesystem
            let name = item.withUnsafeFileSystemRepresentation { pointer -> [UInt8] in
                guard let pointer else { return [] }
                let full = UnsafeBufferPointer(start: UnsafeRawPointer(pointer).assumingMemoryBound(to: UInt8.self),
                                               count: strlen(pointer))
                let slash = full.lastIndex(of: UInt8(ascii: "/")).map { $0 + 1 } ?? 0
                return Array(full[slash...])
            }
            guard !name.isEmpty else { return .unreadable }
            if values.isSymbolicLink == true {
                entries.append(Entry(name: name, kind: TreeBuilder.symlink, size: 0))
            } else if values.isDirectory == true {
                entries.append(Entry(name: name, kind: TreeBuilder.directory, size: 0))
            } else if values.isRegularFile == true {
                guard let bytes = allocated ? values.totalFileAllocatedSize : values.fileSize, bytes >= 0 else {
                    return .unreadable
                }
                entries.append(Entry(name: name, kind: TreeBuilder.file, size: UInt64(bytes)))
            } else {
                entries.append(Entry(name: name, kind: 0, size: 0))
            }
        }
        return .entries(entries)
    }

    // MARK: Scanning new folders

    private static func scanSubtrees(_ urls: [URL], allocated: Bool) async throws -> [ScanTree?] {
        guard !urls.isEmpty else { return [] }
        return try await withThrowingTaskGroup(of: (Int, ScanTree?).self, returning: [ScanTree?].self) { group in
            var results = [ScanTree?](repeating: nil, count: urls.count)
            var next = 0
            while next < urls.count {
                try Task.checkCancellation()
                // At most four helper processes at a time.
                if next >= 4, let (index, tree) = try await group.next() { results[index] = tree }
                let index = next, url = urls[next]
                group.addTask {
                    do { return (index, try await Scanner().scan(url, allocated: allocated) { _ in }) }
                    catch {
                        try Task.checkCancellation()
                        return (index, nil)
                    }
                }
                next += 1
            }
            while let (index, tree) = try await group.next() { results[index] = tree }
            return results
        }
    }
}
