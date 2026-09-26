import Foundation
import Testing
@testable import Vantage

@Suite struct FileRankingTests {
    @Test func eagerRankingMatchesStableReferenceAcrossSizeRanges() throws {
        var random: UInt64 = 572_943
        func next() -> UInt64 {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17
            return random
        }
        func check(_ sizes: [UInt64]) throws {
            var builder = TreeBuilder()
            builder.reserve(sizes.count + sizes.count / 257 + 1)
            var expected: [Int32] = []
            for (i, size) in sizes.enumerated() {
                if i % 257 == 0 {
                    builder.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "folder\(i)".utf8)
                }
                expected.append(builder.append(parent: -1, kind: TreeBuilder.file,
                                               size: size, name: "file\(i)".utf8))
            }
            expected.sort { builder.size[Int($0)] > builder.size[Int($1)] }
            let info = ScanInfo(rootURL: URL(fileURLWithPath: "/fixture"), completion: .complete,
                                reason: "", method: "test", allocated: true, scanDuration: 0, permissionErrors: 0)
            let tree = try ScanTree(info: info, builder: builder)
            #expect(tree.filesBySize == expected)
            #expect(tree.largestFiles(category: nil, matching: "", limit: .max) == expected.map(Int.init))
        }
        try check([])
        try check([7])
        for count in [16_383, 16_384, 16_385] { try check((0..<count).map { _ in next() % (1 << 20) }) }
        try check([UInt64](repeating: 4096, count: 17_000))
        try check((0..<17_000).map { $0 == 8500 ? UInt64(1) << 63 : 0 })
        try check((0..<17_000).map { $0 == 4 ? UInt64.max : 0 })
        try check((0..<70_000).map { _ in (next() % (1 << 22)) << 12 })
        try check((0..<30_000).map { _ in next() % (UInt64.max / 30_000) })
        try check((0..<17_000).map(UInt64.init))
        try check((0..<17_000).reversed().map(UInt64.init))
    }
}
