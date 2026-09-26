import Foundation

struct ItemRow: Identifiable, Hashable, Sendable {
    let id: Int
    let name: String
    let size: UInt64
    let items: Int
    let isDirectory: Bool
    let category: FileCategory
    let location: String
}

/// Metadata describing how a tree was produced.
struct ScanInfo {
    enum Completion: UInt32 { case complete = 0, partial, unsupported, failed }

    var rootURL: URL
    var completion: Completion
    var reason: String
    var method: String
    var allocated: Bool
    var scanDuration: TimeInterval
    var permissionErrors: UInt64
}

/// Accumulates entries for a new `ScanTree`. A parent of -1 is the scanned root.
/// Directory sizes are derived from their files when the tree is built.
struct TreeBuilder {
    static let file: UInt8 = 1, directory: UInt8 = 2, symlink: UInt8 = 5

    private(set) var parent: [Int32] = []
    private(set) var kind: [UInt8] = []
    private(set) var size: [UInt64] = []
    private(set) var nameStart: [Int] = []
    private(set) var nameLength: [UInt32] = []
    private(set) var names: [UInt8] = []

    var count: Int { parent.count }

    mutating func reserve(_ n: Int) {
        parent.reserveCapacity(n); kind.reserveCapacity(n); size.reserveCapacity(n)
        nameStart.reserveCapacity(n); nameLength.reserveCapacity(n); names.reserveCapacity(n * 16)
    }

    @discardableResult
    mutating func append<Name: Collection>(parent p: Int32, kind k: UInt8, size bytes: UInt64, name: Name) -> Int32
    where Name.Element == UInt8 {
        parent.append(p)
        kind.append(k)
        size.append(k == Self.file ? bytes : 0)
        nameStart.append(names.count)
        names.append(contentsOf: name)
        nameLength.append(UInt32(names.count - nameStart[nameStart.count - 1]))
        return Int32(parent.count - 1)
    }

    /// The same binary layout `vantage --export` writes, so saved scans reuse the decoder.
    func serialize(info: ScanInfo) -> Data {
        var files: UInt64 = 0, dirs: UInt64 = 0, total: UInt64 = 0
        for j in 0..<count {
            if kind[j] == Self.file { files += 1; total += size[j] } else if kind[j] == Self.directory { dirs += 1 }
        }
        var writer = ScanExportWriter(info: info, count: count, namesSize: names.count,
                                      totalSize: total, fileCount: files, folderCount: dirs)
        for j in 0..<count {
            writer.append(parent: parent[j] < 0 ? count : Int(parent[j]), kind: kind[j], size: size[j],
                          name: names[nameStart[j]..<(nameStart[j] + Int(nameLength[j]))])
        }
        return writer.data
    }
}

/// A scanned directory tree held as flat arrays so that scans with millions of
/// entries stay compact. Node `count` is the scanned root.
final class ScanTree {
    typealias Completion = ScanInfo.Completion

    let id = UUID()
    let info: ScanInfo
    let count: Int

    private(set) var parent: [Int32]
    private(set) var kind: [UInt8]
    private(set) var isDirectory: [Bool]
    private(set) var size: [UInt64]
    private(set) var items: [Int32]          // descendants (files + folders)
    private(set) var category: [FileCategory]
    private var nameStart: [Int]
    private var nameLength: [UInt32]
    private var names: [UInt8]
    private var childStart: [Int32] = []
    private var children: [Int32] = []
    private var removed: [Bool]
    private(set) var filesBySize: [Int32] = []
    private(set) var categoryTotals: [FileCategory: UInt64] = [:]
    private(set) var freedBytes: UInt64 = 0
    private(set) var fileCount = 0
    private(set) var folderCount = 0

    var rootURL: URL { info.rootURL }
    var completion: Completion { info.completion }
    var reason: String { info.reason }
    var method: String { info.method }
    var allocated: Bool { info.allocated }
    var scanDuration: TimeInterval { info.scanDuration }
    var permissionErrors: UInt64 { info.permissionErrors }
    var root: Int { count }
    var totalSize: UInt64 { size[count] }
    var isPartial: Bool { completion != .complete }

    // MARK: Building

    struct FormatError: LocalizedError {
        var errorDescription: String? { "The scanner returned data this version of Vantage cannot read." }
    }

    /// Decodes `vantage --export` output (or a saved scan in the same format).
    convenience init(export data: Data) throws {
        let (info, builder) = try data.withUnsafeBytes { raw in
            var r = Reader(data: raw)
            guard try r.slice(4).elementsEqual("SVX1".utf8),
                  let completion = Completion(rawValue: try r.u32()) else { throw FormatError() }
            let sizeMode = try r.u32()
            guard sizeMode <= 1 else { throw FormatError() }
            let duration = TimeInterval(try r.u64()) / 1e9
            for _ in 0..<5 { _ = try r.u64() }    // totals are recomputed from entries
            let permissionErrors = try r.u64()
            _ = try r.u64()
            let rootPath = try r.text()
            guard rootPath.hasPrefix("/"), !rootPath.utf8.contains(0) else { throw FormatError() }
            let reason = try r.text()
            let method = try r.text()
            let count = try r.u64()
            // Each record needs 29 fixed bytes and at least one name byte.
            // Check before converting or reserving memory for untrusted counts.
            guard count < UInt64(Int32.max) - 1, count <= UInt64(r.remaining / 30) else { throw FormatError() }
            let n = Int(count)
            var b = TreeBuilder()
            b.reserve(n)
            for j in 0..<n {
                if j & 0x3fff == 0 { try Task.checkCancellation() }
                let p = try r.u64()
                guard p <= count else { throw FormatError() }
                let kind = try r.u8()
                guard kind <= 7 else { throw FormatError() }
                let bytes = try r.u64()
                _ = try r.u64()
                let length = try r.u32()
                let name = try r.slice(Int(length))
                guard !name.isEmpty, !name.contains(0), !name.contains(UInt8(ascii: "/")),
                      !name.elementsEqual(".".utf8), !name.elementsEqual("..".utf8) else { throw FormatError() }
                b.append(parent: p == count ? -1 : Int32(p), kind: kind, size: bytes, name: name)
            }
            guard r.remaining == 0 else { throw FormatError() }
            let info = ScanInfo(rootURL: URL(fileURLWithPath: rootPath, isDirectory: true), completion: completion,
                                reason: reason, method: method, allocated: sizeMode == 1, scanDuration: duration,
                                permissionErrors: permissionErrors)
            return (info, b)
        }
        try self.init(info: info, builder: builder)
    }

    init(info: ScanInfo, builder b: TreeBuilder) throws {
        self.info = info
        let n = b.count
        guard n < Int(Int32.max) - 1 else { throw FormatError() }
        count = n
        parent = b.parent
        for j in 0..<n {
            guard parent[j] >= -1, parent[j] < n else { throw FormatError() }
            if parent[j] == -1 { parent[j] = Int32(n) }
        }
        parent.append(-1)
        kind = b.kind + [TreeBuilder.directory]
        size = b.size + [0]
        nameStart = b.nameStart
        nameLength = b.nameLength
        names = b.names
        let rootName = Array(info.rootURL.lastPathComponent.utf8)
        nameStart.append(names.count)
        nameLength.append(UInt32(rootName.count))
        names.append(contentsOf: rootName)
        isDirectory = kind.map { $0 == TreeBuilder.directory }
        category = .init(repeating: .other, count: n + 1)
        for j in 0...n {
            if kind[j] == TreeBuilder.directory { category[j] = .folder }
            else if kind[j] == TreeBuilder.file {
                category[j] = FileCategory(nameBytes: names[nameStart[j]..<(nameStart[j] + Int(nameLength[j]))])
            }
        }
        removed = .init(repeating: false, count: n + 1)
        items = []
        try buildIndex()
    }

    /// Array storage is shared until either copy is mutated. Background readers
    /// own a snapshot so UI deletions cannot race with queries or cache writes.
    func snapshot() -> Snapshot { Snapshot(tree: ScanTree(copying: self)) }

    /// Only read operations escape this wrapper; its private copy is never mutated.
    struct Snapshot: @unchecked Sendable {
        private let tree: ScanTree

        fileprivate init(tree: ScanTree) { self.tree = tree }

        func folderRows(directory: Int, query: String, limit: Int = 500) -> [ItemRow] {
            let ids = query.isEmpty ? tree.children(of: directory) : tree.search(under: directory, query: query, limit: limit)
            return rows(ids, locations: !query.isEmpty)
        }

        func fileRows(category: FileCategory?, query: String, limit: Int) -> [ItemRow] {
            rows(tree.largestFiles(category: category, matching: query, limit: limit), locations: true)
        }

        private func rows(_ ids: [Int], locations: Bool) -> [ItemRow] {
            ids.map { i in
                ItemRow(id: i, name: tree.name(i), size: tree.size[i], items: Int(tree.items[i]),
                        isDirectory: tree.isDirectory[i], category: tree.category[i],
                        location: locations ? tree.location(i) : "")
            }
        }

        func serialize() -> Data { tree.serialize() }

        func refresh(changes: ChangeJournal.Changes, started: Date) async throws -> IncrementalScan.Result? {
            try await IncrementalScan.refresh(tree, changes: changes, started: started)
        }
    }

    private init(copying tree: ScanTree) {
        info = tree.info; count = tree.count
        parent = tree.parent; kind = tree.kind; isDirectory = tree.isDirectory
        size = tree.size; items = tree.items; category = tree.category
        nameStart = tree.nameStart; nameLength = tree.nameLength; names = tree.names
        childStart = tree.childStart; children = tree.children; removed = tree.removed
        filesBySize = tree.filesBySize; categoryTotals = tree.categoryTotals
        freedBytes = tree.freedBytes; fileCount = tree.fileCount; folderCount = tree.folderCount
    }

    private func buildIndex() throws {
        let n = count
        var start = [Int32](repeating: 0, count: n + 2)
        for j in 0..<n {
            if j & 0x3fff == 0 { try Task.checkCancellation() }
            guard isDirectory[Int(parent[j])] else { throw FormatError() }
            start[Int(parent[j]) + 1] += 1
        }
        for j in 1..<(n + 2) { start[j] += start[j - 1] }
        var fill = start
        var kids = [Int32](repeating: 0, count: n)
        for j in 0..<n {
            let p = Int(parent[j])
            kids[Int(fill[p])] = Int32(j)
            fill[p] += 1
        }
        childStart = start
        children = kids

        // Breadth-first order from the root; reversed, it visits children before parents.
        var order = [Int32](); order.reserveCapacity(n + 1)
        order.append(Int32(n))
        var at = 0
        while at < order.count {
            if at & 0x3fff == 0 { try Task.checkCancellation() }
            let d = Int(order[at]); at += 1
            for k in Int(childStart[d])..<Int(childStart[d + 1]) { order.append(children[k]) }
        }
        guard order.count == n + 1 else { throw FormatError() }   // a cycle or detached entry
        var counts = [Int32](repeating: 0, count: n + 1)
        for node in order.reversed() where Int(node) != n {
            let i = Int(node), p = Int(parent[i])
            counts[p] += counts[i] + 1
            let (total, overflow) = size[p].addingReportingOverflow(size[i])
            guard !overflow else { throw FormatError() }
            size[p] = total
        }
        items = counts
        for d in 0...n where isDirectory[d] { sortChildren(of: d) }

        var files = [Int32]()
        var totals = [UInt64](repeating: 0, count: FileCategory.allCases.count)
        var presentCategories = [Bool](repeating: false, count: FileCategory.allCases.count)
        var folders = 0
        for j in 0..<n {
            if kind[j] == TreeBuilder.file {
                files.append(Int32(j))
                let categoryIndex = Int(category[j].rawValue)
                totals[categoryIndex] += size[j]
                presentCategories[categoryIndex] = true
            } else if isDirectory[j] {
                folders += 1
            }
        }
        sortFilesBySize(&files)
        filesBySize = files
        categoryTotals = Dictionary(uniqueKeysWithValues: FileCategory.allCases.compactMap { category in
            let index = Int(category.rawValue)
            return presentCategories[index] ? (category, totals[index]) : nil
        })
        fileCount = files.count
        folderCount = folders
    }

    private func sortFilesBySize(_ files: inout [Int32]) {
        let sizes = size
        guard files.count >= 16_384 else {
            files.sort { sizes[Int($0)] > sizes[Int($1)] }
            return
        }
        sizes.withUnsafeBufferPointer { sizes in
            let first = sizes[Int(files[0])]
            var variableBits: UInt64 = 0
            for file in files { variableBits |= sizes[Int(file)] ^ first }
            guard variableBits != 0 else { return }
            var scratch = [Int32](repeating: 0, count: files.count)
            var shift = variableBits.trailingZeroBitCount
            // Stable low-to-high digit passes preserve equal-size file order.
            while shift < 64 {
                let width = min(11, 64 - shift)
                let buckets = 1 << width
                let mask = UInt64(buckets - 1)
                if (variableBits >> shift) & mask != 0 {
                    var offsets = [Int](repeating: 0, count: buckets)
                    files.withUnsafeBufferPointer { source in
                        scratch.withUnsafeMutableBufferPointer { destination in
                            offsets.withUnsafeMutableBufferPointer { positions in
                                for file in source {
                                    let bucket = Int(mask - ((sizes[Int(file)] >> shift) & mask))
                                    positions[bucket] += 1
                                }
                                var start = 0
                                for bucket in 0..<buckets {
                                    let count = positions[bucket]
                                    positions[bucket] = start
                                    start += count
                                }
                                for file in source {
                                    let bucket = Int(mask - ((sizes[Int(file)] >> shift) & mask))
                                    destination[positions[bucket]] = file
                                    positions[bucket] += 1
                                }
                            }
                        }
                    }
                    swap(&files, &scratch)
                }
                shift += width
            }
        }
    }

    /// Appends everything below this tree's root under `parent` in another builder.
    func graft(into b: inout TreeBuilder, under parent: Int32) {
        var stack: [(Int, Int32)] = [(root, parent)]
        while let (d, np) = stack.popLast() {
            for c in children(of: d) {
                let ni = b.append(parent: np, kind: kind[c], size: size[c], name: nameBytes(c))
                if isDirectory[c] { stack.append((c, ni)) }
            }
        }
    }

    func serialize() -> Data {
        // Compact live indexes without copying the tree's names and metadata.
        // Build the whole mapping first because parents can follow their children.
        var indexes = [Int32](repeating: -1, count: count + 1)
        var liveCount: Int32 = 0
        var namesSize = 0
        for j in 0..<count where !removed[j] {
            indexes[j] = liveCount
            liveCount += 1
            namesSize += Int(nameLength[j])
        }
        indexes[root] = liveCount
        var writer = ScanExportWriter(info: info, count: Int(liveCount), namesSize: namesSize,
                                      totalSize: totalSize, fileCount: UInt64(fileCount),
                                      folderCount: UInt64(folderCount))
        for j in 0..<count where !removed[j] {
            writer.append(parent: Int(indexes[Int(parent[j])]), kind: kind[j], size: size[j], name: nameBytes(j))
        }
        return writer.data
    }

    func carryFreedBytes(from other: ScanTree) {
        freedBytes += other.freedBytes
    }

    /// Finds an item by its path relative to the root.
    func find(relativePath components: [String]) -> Int? {
        var at = root
        for component in components {
            guard let next = children(of: at).first(where: { name($0) == component }) else { return nil }
            at = next
        }
        return at
    }

    func relativeComponents(_ i: Int) -> [String] {
        lineage(i).dropFirst().map(name)
    }

    func nameBytes(_ i: Int) -> ArraySlice<UInt8> {
        names[nameStart[i]..<(nameStart[i] + Int(nameLength[i]))]
    }

    private func sortChildren(of d: Int) {
        let lo = Int(childStart[d]), hi = Int(childStart[d + 1])
        guard hi - lo > 1 else { return }
        let sizes = size
        children.withUnsafeMutableBufferPointer { buffer in
            var slice = UnsafeMutableBufferPointer(rebasing: buffer[lo..<hi])
            slice.sort { sizes[Int($0)] > sizes[Int($1)] }
        }
    }

    // MARK: Queries

    func name(_ i: Int) -> String {
        let s = nameStart[i]
        return String(decoding: names[s..<(s + Int(nameLength[i]))], as: UTF8.self)
    }

    func children(of d: Int, limit: Int = .max) -> [Int] {
        guard isDirectory[d], limit > 0 else { return [] }
        var result: [Int] = []
        result.reserveCapacity(min(limit, Int(childStart[d + 1] - childStart[d])))
        for k in Int(childStart[d])..<Int(childStart[d + 1]) {
            let child = Int(children[k])
            if !removed[child] {
                result.append(child)
                if result.count == limit { break }
            }
        }
        return result
    }

    func isRemoved(_ i: Int) -> Bool {
        removed[i]
    }

    /// Ancestors from the root down to (and including) `i`.
    func lineage(_ i: Int) -> [Int] {
        var chain = [i]
        var at = i
        while at != count { at = Int(parent[at]); chain.append(at) }
        return chain.reversed()
    }

    func url(_ i: Int) -> URL {
        if i == count { return rootURL }
        var parts: [[UInt8]] = []
        var at = i
        while at != count {
            let s = nameStart[at]
            parts.append(Array(names[s..<(s + Int(nameLength[at]))]))
            at = Int(parent[at])
        }
        var path = Array(rootURL.path.utf8)
        for part in parts.reversed() {
            if path.last != UInt8(ascii: "/") { path.append(UInt8(ascii: "/")) }
            path.append(contentsOf: part)
        }
        path.append(0)
        return path.withUnsafeBufferPointer { buffer in
            buffer.withMemoryRebound(to: CChar.self) {
                URL(fileURLWithFileSystemRepresentation: $0.baseAddress!, isDirectory: isDirectory[i], relativeTo: nil)
            }
        }
    }

    /// Path of the item's folder relative to the scanned root, for display.
    func location(_ i: Int) -> String {
        var parts: [String] = []
        var at = Int(parent[i])
        while at != count { parts.append(name(at)); at = Int(parent[at]) }
        return parts.isEmpty ? rootURL.lastPathComponent : parts.reversed().joined(separator: " ▸ ")
    }

    /// Case-insensitive name matcher; ASCII queries compare bytes directly.
    private struct Matcher {
        let text: String
        let ascii: [UInt8]?

        init(_ query: String) {
            text = query.trimmingCharacters(in: .whitespaces)
            let bytes = Array(text.utf8)
            ascii = bytes.allSatisfy { $0 < 128 } ? bytes.map { $0 >= 65 && $0 <= 90 ? $0 + 32 : $0 } : nil
        }

        var isEmpty: Bool { text.isEmpty }
    }

    private func matches(_ i: Int, _ m: Matcher) -> Bool {
        guard let needle = m.ascii else { return name(i).localizedStandardContains(m.text) }
        let s = nameStart[i], n = Int(nameLength[i]), k = needle.count
        guard k <= n else { return false }
        return names.withUnsafeBufferPointer { buffer in
            var start = s
            while start + k <= s + n {
                var j = 0
                while j < k {
                    var b = buffer[start + j]
                    if b >= 65 && b <= 90 { b += 32 }
                    if b != needle[j] { break }
                    j += 1
                }
                if j == k { return true }
                start += 1
            }
            return false
        }
    }

    /// Largest files, optionally filtered, stopping after `limit` matches.
    func largestFiles(category filter: FileCategory?, matching query: String, limit: Int) -> [Int] {
        guard limit > 0 else { return [] }
        let m = Matcher(query)
        var out: [Int] = []
        for f in filesBySize {
            let i = Int(f)
            if i & 0xfff == 0 && Task.isCancelled { return [] }
            if let filter, category[i] != filter { continue }
            if isRemoved(i) { continue }
            if !m.isEmpty && !matches(i, m) { continue }
            out.append(i)
            if out.count >= limit { break }
        }
        return out
    }

    /// Items anywhere below `directory` whose names match, largest first.
    func search(under directory: Int, query: String, limit: Int) -> [Int] {
        guard limit > 0 else { return [] }
        let m = Matcher(query)
        guard !m.isEmpty else { return children(of: directory) }
        var found: [Int] = []
        // Keep only the largest `limit` matches in a min-heap. Common queries
        // no longer allocate and sort an array containing the entire scan.
        func better(_ a: Int, than b: Int) -> Bool {
            size[a] != size[b] ? size[a] > size[b] : a < b
        }
        func siftDown() {
            var at = 0
            while at * 2 + 1 < found.count {
                var child = at * 2 + 1
                if child + 1 < found.count && better(found[child], than: found[child + 1]) { child += 1 }
                if !better(found[at], than: found[child]) { break }
                found.swapAt(at, child); at = child
            }
        }
        var stack = [directory]
        while let d = stack.popLast() {
            for k in Int(childStart[d])..<Int(childStart[d + 1]) {
                if k & 0xfff == 0 && Task.isCancelled { return [] }
                let c = Int(children[k])
                if removed[c] { continue }
                if matches(c, m) {
                    if found.count < limit {
                        found.append(c)
                        var at = found.count - 1
                        while at > 0 {
                            let p = (at - 1) / 2
                            if !better(found[p], than: found[at]) { break }
                            found.swapAt(p, at); at = p
                        }
                    } else if better(c, than: found[0]) {
                        found[0] = c
                        siftDown()
                    }
                }
                if isDirectory[c] { stack.append(c) }
            }
        }
        found.sort { better($0, than: $1) }
        return found
    }

    // MARK: Mutation

    /// Records that an item no longer exists on disk and updates every total.
    func remove(_ i: Int) {
        guard i != count, !isRemoved(i) else { return }
        let bytes = size[i]
        let descendants = items[i] + 1
        removed[i] = true
        freedBytes += bytes
        var stack = [i]
        while let node = stack.popLast() {
            removed[node] = true
            if isDirectory[node] {
                folderCount -= 1
                for k in Int(childStart[node])..<Int(childStart[node + 1]) where !removed[Int(children[k])] {
                    stack.append(Int(children[k]))
                }
            } else if kind[node] == TreeBuilder.file {
                fileCount -= 1
                let c = category[node]
                categoryTotals[c] = (categoryTotals[c] ?? 0) - min(categoryTotals[c] ?? 0, size[node])
            }
        }
        var at = Int(parent[i])
        while true {
            size[at] -= min(size[at], bytes)
            items[at] -= min(items[at], descendants)
            sortChildren(of: at)
            if at == count { break }
            at = Int(parent[at])
        }
    }
}

private struct Reader {
    let data: UnsafeRawBufferPointer
    var offset = 0
    var remaining: Int { data.count - offset }

    struct Truncated: Error {}

    mutating func slice(_ n: Int) throws -> UnsafeRawBufferPointer {
        guard n >= 0, n <= remaining else { throw Truncated() }
        defer { offset += n }
        return UnsafeRawBufferPointer(rebasing: data[offset ..< offset + n])
    }
    mutating func u8() throws -> UInt8 {
        guard offset < data.count else { throw Truncated() }
        defer { offset += 1 }
        return data[offset]
    }
    mutating func u32() throws -> UInt32 {
        guard remaining >= 4 else { throw Truncated() }
        let v = data.loadUnaligned(fromByteOffset: offset, as: UInt32.self)
        offset += 4
        return UInt32(littleEndian: v)
    }
    mutating func u64() throws -> UInt64 {
        guard remaining >= 8 else { throw Truncated() }
        let v = data.loadUnaligned(fromByteOffset: offset, as: UInt64.self)
        offset += 8
        return UInt64(littleEndian: v)
    }
    mutating func text() throws -> String {
        let n = Int(try u32())
        return String(decoding: try slice(n), as: UTF8.self)
    }
}
