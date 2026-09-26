import Foundation
import Testing
@testable import Vantage

@Suite struct FileCategoryTests {
    @Test func classifiesRawNameSlicesAtExtensionBoundaries() {
        let cases: [([UInt8], FileCategory)] = [
            (Array("movie.MP4".utf8), .video),
            (Array("project.fcpbundle".utf8), .video),       // nine bytes
            (Array("library.framework".utf8), .application),
            (Array("library.component".utf8), .application),
            (Array("file.aaaaaaaaaa".utf8), .other),        // ten bytes
            (Array("disk.sparseimage".utf8), .archive),     // eleven bytes
            (Array("disk.sparsebundle".utf8), .other),      // twelve-byte extensions remain excluded
            (Array("debug.dSYM".utf8), .developer),
            (Array(".mp4".utf8), .other),
            (Array(".hidden.JPG".utf8), .image),
            (Array("file..pdf".utf8), .document),
            (Array("trailing.".utf8), .other),
            (Array("noextension".utf8), .other),
            ([], .other),
            ([0x80, 46, 112, 100, 102], .document),
            ([102, 46, 0x80, 112, 100, 102], .other),
            ([102, 46, 0, 112, 100, 102], .other),
            ([102, 46, 112, 100, 102, 0], .other),
            ([102, 46] + [UInt8](repeating: 127, count: 9), .other),
        ]
        for (name, expected) in cases {
            #expect(FileCategory(nameBytes: name[...]) == expected)
            let padded = [UInt8(46), 112, 100, 102, 255] + name + [46, 109, 112, 52]
            #expect(FileCategory(nameBytes: padded[5..<(5 + name.count)]) == expected)
        }
    }

    @Test func keepsZeroByteCategoriesAndOmitsAbsentCategories() throws {
        let info = ScanInfo(rootURL: URL(fileURLWithPath: "/fixture"), completion: .complete,
                            reason: "", method: "test", allocated: true, scanDuration: 0, permissionErrors: 0)
        var builder = TreeBuilder()
        builder.append(parent: -1, kind: TreeBuilder.file, size: 0, name: "zero.MP4".utf8)
        builder.append(parent: -1, kind: TreeBuilder.file, size: 0, name: "zero.unknown".utf8)
        builder.append(parent: -1, kind: TreeBuilder.file, size: 1, name: "one.pdf".utf8)
        builder.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "folder.mov".utf8)
        builder.append(parent: -1, kind: TreeBuilder.symlink, size: 0, name: "link.mp3".utf8)
        let tree = try ScanTree(info: info, builder: builder)
        #expect(tree.categoryTotals == [.video: 0, .other: 0, .document: 1])
        #expect(tree.categoryTotals[.audio] == nil)
        #expect(tree.categoryTotals[.folder] == nil)
        let empty = try ScanTree(info: info, builder: TreeBuilder())
        #expect(empty.categoryTotals.isEmpty)
        var onlyDirectory = TreeBuilder()
        onlyDirectory.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "folder.mp4".utf8)
        let noFiles = try ScanTree(info: info, builder: onlyDirectory)
        #expect(noFiles.categoryTotals.isEmpty)
    }
}
