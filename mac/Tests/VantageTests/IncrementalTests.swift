import Foundation
import Testing
@testable import Vantage

/// Incremental refreshes must produce exactly what a fresh full scan would.
@Suite(.serialized) struct IncrementalTests {
    let root: URL
    let fm = FileManager.default

    init() throws {
        root = URL(fileURLWithPath: ScanCache.canonical(fm.temporaryDirectory))
            .appendingPathComponent("vantage-inc-\(UUID().uuidString)", isDirectory: true)
        for dir in ["A/deep/deeper", "B", "C/keep"] {
            try fm.createDirectory(at: root.appendingPathComponent(dir), withIntermediateDirectories: true)
        }
        try write("A/one.bin", 1_000)
        try write("A/deep/two.bin", 2_000)
        try write("A/deep/deeper/three.bin", 3_000)
        try write("B/gone.bin", 4_000)
        try write("B/grows.bin", 10)
        try write("C/keep/stay.bin", 5_000)
    }

    func write(_ path: String, _ bytes: Int) throws {
        try Data(count: bytes).write(to: root.appendingPathComponent(path))
    }

    /// Every entry as "relative/path kind size" for exact comparison.
    func inventory(_ tree: ScanTree) -> [String] {
        var out: [String] = []
        var stack = [(tree.root, "")]
        while let (d, prefix) = stack.popLast() {
            for c in tree.children(of: d) {
                let path = prefix + tree.name(c)
                out.append("\(path) \(tree.kind[c]) \(tree.size[c]) \(tree.items[c])")
                if tree.isDirectory[c] { stack.append((c, path + "/")) }
            }
        }
        return out.sorted()
    }

    @Test func refreshMatchesFullScan() async throws {
        defer { try? fm.removeItem(at: root) }
        let mark = ChangeJournal.mark(for: root)
        let before = try await Scanner().scan(root, allocated: false) { _ in }
        try await Task.sleep(for: .milliseconds(200))

        try write("A/added.bin", 700)                                   // new file
        try fm.removeItem(at: root.appendingPathComponent("B/gone.bin")) // deleted file
        try write("B/grows.bin", 9_000)                                 // changed size
        try fm.createDirectory(at: root.appendingPathComponent("New/inner"), withIntermediateDirectories: true)
        try write("New/inner/fresh.bin", 6_000)                         // new nested folder
        try write("A/deep/deeper/four.bin", 400)                        // deep change
        try fm.removeItem(at: root.appendingPathComponent("C/keep"))    // deleted folder
        try await Task.sleep(for: .seconds(1.5))                        // let FSEvents record them

        let changes = await ChangeJournal.changes(under: root, since: mark)
        #expect(!changes.needsFullScan)
        #expect(changes.shallow.contains(root.appendingPathComponent("A/deep/deeper").path))
        let result = try #require(try await IncrementalScan.refresh(before, changes: changes))
        let fresh = try await Scanner().scan(root, allocated: false) { _ in }

        #expect(inventory(result.tree) == inventory(fresh))
        #expect(result.tree.totalSize == fresh.totalSize)
        #expect(result.tree.fileCount == fresh.fileCount)
        #expect(result.tree.folderCount == fresh.folderCount)
        #expect(result.subtreesScanned == 1)          // only "New" needed the scanner
    }

    @Test func savedScanRoundTrips() async throws {
        defer { try? fm.removeItem(at: root) }
        let tree = try await Scanner().scan(root, allocated: true) { _ in }
        let reloaded = try ScanTree(export: tree.serialize())
        #expect(inventory(reloaded) == inventory(tree))
        #expect(reloaded.allocated && reloaded.totalSize == tree.totalSize)

        let record = ScanCache.Record(root: tree.rootURL.path, allocated: true, mark: ChangeJournal.mark(for: root),
                                      scannedAt: Date(), totalSize: tree.totalSize, fileCount: tree.fileCount)
        ScanCache.save(tree.serialize(), record: record)
        defer { ScanCache.forget(record) }
        let found = try #require(ScanCache.record(for: root, allocated: true))
        #expect(found == record)
        #expect(inventory(try ScanCache.load(found)) == inventory(tree))
        #expect(ScanCache.record(for: root, allocated: false) == nil)
    }

    @Test func quietFolderNeedsNoWork() async throws {
        defer { try? fm.removeItem(at: root) }
        _ = try await Scanner().scan(root, allocated: false) { _ in }
        try await Task.sleep(for: .seconds(1))
        let mark = ChangeJournal.mark(for: root)
        let changes = await ChangeJournal.changes(under: root, since: mark)
        #expect(!changes.needsFullScan && changes.shallow.isEmpty && changes.deep.isEmpty)
    }

    @Test func partialTreesAndReplacedDirectoriesRequireAFullScan() async throws {
        defer { try? fm.removeItem(at: root) }
        let before = try await Scanner().scan(root, allocated: false) { _ in }
        var builder = TreeBuilder()
        before.graft(into: &builder, under: -1)
        var info = before.info
        info.completion = .partial
        let partial = try ScanTree(info: info, builder: builder)
        #expect(try await IncrementalScan.refresh(partial, changes: .init()) == nil)

        let a = root.appendingPathComponent("A")
        try fm.removeItem(at: a)
        try fm.createSymbolicLink(at: a, withDestinationURL: root.appendingPathComponent("B"))
        #expect(try await IncrementalScan.refresh(before, changes: .init(shallow: [a.path])) == nil)
    }

    @Test func staleCacheWritersAndReadersCannotReplaceOrEraseANewerSnapshot() async throws {
        defer { try? fm.removeItem(at: root) }
        let first = try await Scanner().scan(root, allocated: false) { _ in }
        let firstRecord = ScanCache.Record(root: first.rootURL.path, allocated: false, mark: ChangeJournal.mark(for: root),
                                          scannedAt: Date(timeIntervalSince1970: 1000),
                                          totalSize: first.totalSize, fileCount: first.fileCount)
        ScanCache.save(first.serialize(), record: firstRecord)
        try write("new.bin", 100)
        let second = try await Scanner().scan(root, allocated: false) { _ in }
        let secondRecord = ScanCache.Record(root: second.rootURL.path, allocated: false, mark: ChangeJournal.mark(for: root),
                                           scannedAt: Date(timeIntervalSince1970: 2000),
                                           totalSize: second.totalSize, fileCount: second.fileCount)
        defer { ScanCache.forget(firstRecord); ScanCache.forget(secondRecord) }
        ScanCache.save(second.serialize(), record: secondRecord)
        ScanCache.save(first.serialize(), record: firstRecord)
        ScanCache.forget(firstRecord)
        #expect(ScanCache.record(for: root, allocated: false) == secondRecord)
        #expect(inventory(try ScanCache.load(secondRecord)) == inventory(second))
        #expect(throws: (any Error).self) { try ScanCache.load(firstRecord) }

        // Existing caches without a generation remain readable.
        var legacy = secondRecord
        legacy.snapshot = nil
        ScanCache.save(second.serialize(), record: legacy)
        defer { ScanCache.forget(legacy) }
        #expect(inventory(try ScanCache.load(legacy)) == inventory(second))
    }
}
