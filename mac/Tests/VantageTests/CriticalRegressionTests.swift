import Foundation
import Testing
@testable import Vantage

@Suite struct CriticalRegressionTests {
    private func info(root: URL = URL(fileURLWithPath: "/tmp/vantage-regression")) -> ScanInfo {
        ScanInfo(rootURL: root, completion: .complete, reason: "", method: "fixture",
                 allocated: false, scanDuration: 0, permissionErrors: 0)
    }

    @Test func rejectsCorruptExportsWithoutCrashingOrAllocatingFromTheirCounts() throws {
        var builder = TreeBuilder()
        builder.append(parent: -1, kind: 2, size: 0, name: "folder".utf8)
        builder.append(parent: 0, kind: 1, size: 7, name: "one".utf8)
        builder.append(parent: 0, kind: 1, size: 9, name: "two".utf8)
        let data = builder.serialize(info: info())

        for end in 0..<data.count {
            #expect(throws: (any Error).self) { try ScanTree(export: data.prefix(end)) }
        }
        func write64(_ value: UInt64, at offset: Int, in bytes: inout Data) {
            withUnsafeBytes(of: value.littleEndian) { bytes.replaceSubrange(offset..<(offset + 8), with: $0) }
        }
        let countOffset = 76 + 12 + info().rootURL.path.utf8.count + info().method.utf8.count
        let first = countOffset + 8
        for count in [UInt64.max, UInt64(Int32.max), 1_000_000] {
            var damaged = data
            write64(count, at: countOffset, in: &damaged)
            #expect(throws: ScanTree.FormatError.self) { try ScanTree(export: damaged) }
        }
        var cycle = data
        write64(0, at: first, in: &cycle) // the folder is its own parent
        #expect(throws: ScanTree.FormatError.self) { try ScanTree(export: cycle) }

        var fileParent = data
        write64(1, at: first + 29 + 6 + 29 + 3, in: &fileParent)
        #expect(throws: ScanTree.FormatError.self) { try ScanTree(export: fileParent) }

        var overflow = data
        write64(.max, at: first + 29 + 6 + 9, in: &overflow)
        #expect(throws: ScanTree.FormatError.self) { try ScanTree(export: overflow) }

        for name in ["", ".", "..", "../outside", "nul\0tail"] {
            var invalidName = TreeBuilder()
            invalidName.append(parent: -1, kind: 1, size: 1, name: name.utf8)
            #expect(throws: ScanTree.FormatError.self) {
                try ScanTree(export: invalidName.serialize(info: info()))
            }
        }
        #expect(throws: ScanTree.FormatError.self) { try ScanTree(export: data + Data([0])) }
    }

    @Test func savedTreesPreserveLiveHierarchyAndRawNamesAfterDeletion() throws {
        var builder = TreeBuilder()
        builder.append(parent: 4, kind: 1, size: 9, name: [0xff, 0x61]) // parent follows child
        builder.append(parent: -1, kind: 2, size: 0, name: "removed-folder".utf8)
        builder.append(parent: 1, kind: 1, size: 11, name: "removed-child".utf8)
        builder.append(parent: -1, kind: 1, size: 13, name: "removed-file".utf8)
        builder.append(parent: -1, kind: 2, size: 0, name: "kept-folder".utf8)
        builder.append(parent: 4, kind: 2, size: 0, name: "nested".utf8)
        builder.append(parent: 5, kind: 5, size: 0, name: "link".utf8)
        builder.append(parent: 5, kind: 1, size: 17, name: "file.bin".utf8)
        builder.append(parent: -1, kind: 2, size: 0, name: "empty-folder".utf8)
        var metadata = info()
        metadata.completion = .partial
        metadata.reason = "fixture permission failure"
        metadata.permissionErrors = 5
        metadata.allocated = true
        metadata.scanDuration = 1.25
        let tree = try ScanTree(info: metadata, builder: builder)
        tree.remove(1)
        tree.remove(3)

        struct Record: Hashable {
            var path: [Data]
            var kind: UInt8
            var size: UInt64
            var items: Int32
        }
        func records(_ tree: ScanTree) -> Set<Record> {
            Set((0..<tree.count).filter { !tree.isRemoved($0) }.map { i in
                Record(path: tree.lineage(i).dropFirst().map { Data(tree.nameBytes($0)) },
                       kind: tree.kind[i], size: tree.size[i], items: tree.items[i])
            })
        }
        let reopened = try ScanTree(export: tree.serialize())
        #expect(records(reopened) == records(tree))
        #expect(reopened.count == 6 && reopened.totalSize == 26)
        #expect(reopened.fileCount == 2 && reopened.folderCount == 3)
        #expect(reopened.categoryTotals == tree.categoryTotals)
        #expect(reopened.rootURL.path == tree.rootURL.path && reopened.completion == .partial)
        #expect(reopened.reason == metadata.reason && reopened.permissionErrors == 5)
        #expect(reopened.allocated && reopened.scanDuration == 1.25 && reopened.method == metadata.method)

        for child in tree.children(of: tree.root) { tree.remove(child) }
        let empty = try ScanTree(export: tree.serialize())
        #expect(empty.count == 0 && empty.totalSize == 0 && empty.children(of: empty.root).isEmpty)
    }

    @Test func boundedSearchMatchesFullRankingAndSnapshotsSurviveDeletion() async throws {
        var builder = TreeBuilder()
        let directory = builder.append(parent: -1, kind: 2, size: 0, name: "folder".utf8)
        for i in 0..<20_000 {
            builder.append(parent: i % 3 == 0 ? -1 : directory, kind: 1,
                           size: UInt64((i * 7919) % 3000), name: "payload-\(i).bin".utf8)
        }
        let tree = try ScanTree(info: info(), builder: builder)
        let snapshot = tree.snapshot()
        let total = tree.totalSize
        func reference(under parent: Int) -> [Int] {
            (0..<tree.count).filter {
                tree.kind[$0] == 1 && !tree.isRemoved($0) && (parent == tree.root || tree.parent[$0] == parent)
            }.sorted { tree.size[$0] != tree.size[$1] ? tree.size[$0] > tree.size[$1] : $0 < $1 }
        }
        for parent in [tree.root, Int(directory)] {
            let expected = reference(under: parent)
            for limit in [1, 500, 25_000] {
                #expect(tree.search(under: parent, query: "PAYLOAD", limit: limit) == Array(expected.prefix(limit)))
            }
        }
        let background = Task.detached { try ScanTree(export: snapshot.serialize()) }
        tree.remove(Int(directory))
        #expect(tree.search(under: tree.root, query: "payload", limit: 500) == Array(reference(under: tree.root).prefix(500)))
        #expect(tree.largestFiles(category: nil, matching: "", limit: 0).isEmpty)
        let saved = try await background.value
        #expect(saved.totalSize == total && saved.fileCount == 20_000)
        #expect(tree.fileCount == 6667 && tree.totalSize < total)
        #expect(snapshot.fileRows(category: nil, query: "", limit: 25_000).count == 20_000)
    }

    @Test @MainActor func staleDeletionCannotDeleteAnItemFromAReplacementTree() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("vantage-deletion-\(UUID())")
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        try Data([1]).write(to: root.appendingPathComponent("first"))
        try Data([2]).write(to: root.appendingPathComponent("second"))

        var first = TreeBuilder(), second = TreeBuilder()
        first.append(parent: -1, kind: 1, size: 1, name: "first".utf8)
        second.append(parent: -1, kind: 1, size: 1, name: "second".utf8)
        let model = AppModel()
        func show(_ builder: TreeBuilder) throws {
            let tree = try ScanTree(info: info(root: root), builder: builder)
            let record = ScanCache.Record(root: root.path, allocated: false,
                                          mark: .init(eventID: 0, volumeUUID: nil), scannedAt: Date(),
                                          totalSize: tree.totalSize, fileCount: tree.fileCount)
            model.show(tree, record: record, keepPlace: false)
        }
        try show(first)
        model.requestDeletion([0], permanently: true)
        let request = try #require(model.pendingDeletion)
        try show(second)
        model.performDeletion(request)
        #expect(fm.fileExists(atPath: root.appendingPathComponent("first").path))
        #expect(fm.fileExists(atPath: root.appendingPathComponent("second").path))
        #expect(model.tree?.fileCount == 1)
    }

    @Test func treemapTilesStayFiniteAndWithinTheirBounds() throws {
        var b = TreeBuilder()
        for d in 0..<20 {
            let p = b.append(parent: -1, kind: 2, size: 0, name: "folder-\(d)".utf8)
            for f in 0..<100 {
                b.append(parent: p, kind: 1, size: UInt64((f + 1) * (d + 1)), name: "file-\(f)".utf8)
            }
        }
        let tree = try ScanTree(info: info(), builder: b)
        for size in [CGSize(width: 800, height: 400), CGSize(width: 80, height: 900), CGSize(width: 4, height: 4)] {
            let bounds = CGRect(origin: .zero, size: size)
            let tiles = TreemapLayout.tiles(tree: tree, directory: tree.root, in: bounds)
            if size.width > 40 && size.height > 40 { #expect(!tiles.isEmpty) }
            for tile in tiles {
                #expect(tile.rect.width.isFinite && tile.rect.height.isFinite)
                #expect(tile.rect.width > 0 && tile.rect.height > 0)
                #expect(bounds.insetBy(dx: -0.001, dy: -0.001).contains(tile.rect))
            }
            let top = tiles.filter { $0.depth == 0 }
            for i in top.indices {
                for j in top.indices where j > i { #expect(!top[i].rect.intersects(top[j].rect)) }
            }
        }
    }
}

@Suite struct ScannerLifecycleTests {
    @Test(.timeLimit(.minutes(1))) func taskCancellationTerminatesAndReapsTheHelper() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("vantage-cancel-\(UUID())")
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let helper = root.appendingPathComponent("helper"), pidFile = root.appendingPathComponent("pid")
        let script = "#!/bin/sh\necho $$ > '\(pidFile.path)'\nexec /bin/sleep 30\n"
        try Data(script.utf8).write(to: helper)
        try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: helper.path)
        let scanner = Scanner(helperURL: helper)
        let work = Task { try await scanner.scan(root, allocated: false) { _ in } }
        defer { work.cancel(); scanner.cancel() }
        for _ in 0..<300 {
            if fm.fileExists(atPath: pidFile.path) { break }
            try await Task.sleep(for: .milliseconds(10))
        }
        let pid = try #require(Int32(String(contentsOf: pidFile, encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)))
        let start = ContinuousClock.now
        work.cancel()
        do {
            _ = try await work.value
            Issue.record("A cancelled scan returned a result")
        } catch is CancellationError {
        } catch let error as Vantage.Scanner.Failure {
            guard case .cancelled = error else { throw error }
        }
        #expect(start.duration(to: .now) < .seconds(3))
        #expect(kill(pid, 0) == -1 && errno == ESRCH)

        try fm.removeItem(at: pidFile)
        let cancelledBeforeLaunch = Scanner(helperURL: helper)
        cancelledBeforeLaunch.cancel()
        await #expect(throws: Scanner.Failure.self) {
            try await cancelledBeforeLaunch.scan(root, allocated: false) { _ in }
        }
        #expect(!fm.fileExists(atPath: pidFile.path))
    }

    @Test func failedHelpersDeliverTheirFinalStderrMessage() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("vantage-stderr-\(UUID())")
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let helper = root.appendingPathComponent("helper")
        try Data("#!/bin/sh\nprintf 'vantage: fixture failure' >&2\nexit 71\n".utf8).write(to: helper)
        try fm.setAttributes([.posixPermissions: 0o700], ofItemAtPath: helper.path)
        for _ in 0..<10 {
            do {
                _ = try await Scanner(helperURL: helper).scan(root, allocated: false) { _ in }
                Issue.record("The failed helper was accepted")
            } catch let error as Vantage.Scanner.Failure {
                guard case .scanner(let message) = error else { throw error }
                #expect(message == "Fixture failure")
            }
        }
    }
}
