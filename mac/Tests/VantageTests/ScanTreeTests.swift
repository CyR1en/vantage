import Foundation
import Testing
@testable import Vantage

/// Exercises the real scanner helper (../build/vantage) on a small fixture.
@Suite struct ScanTreeTests {
    let root: URL

    init() throws {
        root = FileManager.default.temporaryDirectory
            .appendingPathComponent("vantage-tests-\(UUID().uuidString)", isDirectory: true)
        let fm = FileManager.default
        try fm.createDirectory(at: root.appendingPathComponent("Movies/Trips"), withIntermediateDirectories: true)
        try fm.createDirectory(at: root.appendingPathComponent("Docs"), withIntermediateDirectories: true)
        try Data(count: 5_000).write(to: root.appendingPathComponent("Movies/Trips/Beach.mov"))
        try Data(count: 3_000).write(to: root.appendingPathComponent("Movies/Clip.MP4"))
        try Data(count: 1_000).write(to: root.appendingPathComponent("Docs/Notes.pdf"))
        try Data(count: 10).write(to: root.appendingPathComponent("readme"))
    }

    func scan() async throws -> ScanTree {
        return try await Scanner().scan(root, allocated: false) { _ in }
    }

    func find(_ tree: ScanTree, _ name: String) -> Int? {
        (0..<tree.count).first { tree.name($0) == name }
    }

    @Test func parsesSizesAndStructure() async throws {
        defer { try? FileManager.default.removeItem(at: root) }
        let tree = try await scan()
        #expect(tree.completion == .complete)
        #expect(tree.totalSize == 9_010)
        #expect(tree.fileCount == 4)
        #expect(tree.folderCount == 3)
        let top = tree.children(of: tree.root).map(tree.name)
        #expect(top == ["Movies", "Docs", "readme"])
        let movies = try #require(find(tree, "Movies"))
        #expect(tree.size[movies] == 8_000)
        #expect(tree.items[movies] == 3)
        let beach = try #require(find(tree, "Beach.mov"))
        #expect(tree.category[beach] == .video)
        #expect(tree.category[try #require(find(tree, "Clip.MP4"))] == .video)
        #expect(tree.url(beach).path.hasSuffix("/Movies/Trips/Beach.mov"))
        #expect(FileManager.default.fileExists(atPath: tree.url(beach).path))
        #expect(tree.categoryTotals[.video] == 8_000)
    }

    @Test func searchesAndRanksFiles() async throws {
        defer { try? FileManager.default.removeItem(at: root) }
        let tree = try await scan()
        #expect(tree.largestFiles(category: nil, matching: "", limit: 2).map(tree.name) == ["Beach.mov", "Clip.MP4"])
        #expect(tree.largestFiles(category: .document, matching: "", limit: 10).map(tree.name) == ["Notes.pdf"])
        #expect(tree.search(under: tree.root, query: "clip", limit: 10).map(tree.name) == ["Clip.MP4"])
        #expect(tree.search(under: tree.root, query: "TRIP", limit: 10).map(tree.name) == ["Trips"])
    }

    @Test func removalUpdatesEveryTotal() async throws {
        defer { try? FileManager.default.removeItem(at: root) }
        let tree = try await scan()
        let trips = try #require(find(tree, "Trips"))
        let movies = try #require(find(tree, "Movies"))
        tree.remove(trips)
        #expect(tree.isRemoved(try #require(find(tree, "Beach.mov"))))
        #expect(tree.size[movies] == 3_000)
        #expect(tree.items[movies] == 1)
        #expect(tree.totalSize == 4_010)
        #expect(tree.freedBytes == 5_000)
        #expect(tree.fileCount == 3)
        #expect(tree.folderCount == 2)
        #expect(tree.categoryTotals[.video] == 3_000)
        #expect(tree.largestFiles(category: nil, matching: "", limit: 1).map(tree.name) == ["Clip.MP4"])
        // Sibling order follows the new sizes: Movies (3,000) now trails nothing larger.
        #expect(tree.children(of: tree.root).map(tree.name) == ["Movies", "Docs", "readme"])
        tree.remove(movies)
        #expect(tree.children(of: tree.root).map(tree.name) == ["Docs", "readme"])
        #expect(tree.totalSize == 1_010)
    }

    @Test func actualDiskUsageIgnoresSparseImages() async throws {
        defer { try? FileManager.default.removeItem(at: root) }
        // Like a VM disk: 64 GiB apparent length, almost nothing allocated.
        let image = root.appendingPathComponent("Movies/data.img.raw")
        FileManager.default.createFile(atPath: image.path, contents: nil)
        let handle = try FileHandle(forWritingTo: image)
        try handle.truncate(atOffset: 64 << 30)
        try handle.close()
        let apparent = try await Scanner().scan(root, allocated: false) { _ in }
        let actual = try await Scanner().scan(root, allocated: true) { _ in }
        #expect(apparent.totalSize > 64 << 30)
        #expect(actual.allocated)
        #expect(actual.totalSize < 10 << 20)
        #expect(actual.category[try #require(find(actual, "data.img.raw"))] == .archive)
    }

    @Test func reportsScannerErrors() async throws {
        defer { try? FileManager.default.removeItem(at: root) }
        await #expect(throws: Scanner.Failure.self) {
            try await Scanner().scan(root.appendingPathComponent("missing"), allocated: false) { _ in }
        }
    }
}
