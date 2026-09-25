import AppKit
import SwiftUI
import Testing
@testable import Vantage

@Suite(.serialized) @MainActor struct BrowserRefreshTests {
    private func tree(files: Int) throws -> ScanTree {
        var builder = TreeBuilder()
        for index in 0..<files {
            builder.append(parent: -1, kind: TreeBuilder.file, size: UInt64(index + 1),
                           name: "file-\(index).bin".utf8)
        }
        let info = ScanInfo(rootURL: URL(fileURLWithPath: "/tmp/vantage-browser-refresh"),
                            completion: .complete, reason: "", method: "fixture", allocated: true,
                            scanDuration: 0, permissionErrors: 0)
        return try ScanTree(info: info, builder: builder)
    }

    private func record(for tree: ScanTree) -> ScanCache.Record {
        ScanCache.Record(root: tree.rootURL.path, allocated: tree.allocated,
                         mark: .init(eventID: 0, volumeUUID: nil), scannedAt: Date(),
                         totalSize: tree.totalSize, fileCount: tree.fileCount)
    }

    @Test(arguments: [2, 0, 1])
    func replacingVisibleTreeWhileShowingRefreshToast(fileCount: Int) async throws {
        _ = NSApplication.shared
        let model = AppModel()
        let before = try tree(files: 1)
        model.show(before, record: record(for: before), keepPlace: false)
        let host = NSHostingView(rootView: ContentView(model: model))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 760),
                              styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        defer { model.cancelScan(); window.close() }
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(200))

        let after = try tree(files: fileCount)
        model.show(after, record: record(for: after), keepPlace: true)
        model.showToast("Updated")
        host.layoutSubtreeIfNeeded()
        try await Task.sleep(for: .milliseconds(200))

        #expect(model.tree === after)
        #expect(model.directory == after.root)
        #expect(window.title == after.name(after.root))
    }

    @Test func replacementRemapsNavigationWithoutChangingTheRetiringBrowser() throws {
        var beforeBuilder = TreeBuilder()
        let a = beforeBuilder.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "A".utf8)
        let keep = beforeBuilder.append(parent: a, kind: TreeBuilder.file, size: 1, name: "keep.bin".utf8)
        let b = beforeBuilder.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "B".utf8)
        beforeBuilder.append(parent: b, kind: TreeBuilder.file, size: 2, name: "gone.bin".utf8)
        let info = try tree(files: 0).info
        let before = try ScanTree(info: info, builder: beforeBuilder)
        let model = AppModel()
        model.show(before, record: record(for: before), keepPlace: false)
        model.navigate(to: Int(a))
        model.navigate(to: Int(b))
        model.goBack()
        model.selection = [Int(keep)]
        let retiring = try #require(model.browser)

        // The count is unchanged, but the same indices now refer to different
        // files and folders. Bounds checks alone would accept the wrong items.
        var afterBuilder = TreeBuilder()
        afterBuilder.append(parent: -1, kind: TreeBuilder.file, size: 3, name: "added.bin".utf8)
        let newB = afterBuilder.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "B".utf8)
        let newA = afterBuilder.append(parent: -1, kind: TreeBuilder.directory, size: 0, name: "A".utf8)
        let newKeep = afterBuilder.append(parent: newA, kind: TreeBuilder.file, size: 1, name: "keep.bin".utf8)
        let after = try ScanTree(info: info, builder: afterBuilder)
        model.show(after, record: record(for: after), keepPlace: true)

        #expect(model.browser !== retiring)
        #expect(model.directory == Int(newA))
        #expect(model.selectedItems == [Int(newKeep)])
        #expect(model.canGoBack && model.canGoForward)
        model.goForward()
        #expect(model.directory == Int(newB))
        model.goBack()
        #expect(model.directory == Int(newA))
        model.goUp()
        #expect(model.directory == after.root && model.selection == [Int(newA)])

        #expect(retiring.tree === before)
        #expect(retiring.directory == Int(a))
        #expect(retiring.selectedItems == [Int(keep)])
        #expect(retiring.backStack == [before.root])
        #expect(retiring.forwardStack == [Int(b)])
        #expect(retiring.tree.name(retiring.directory) == "A")

        model.navigate(to: Int(newA))
        model.selection = [Int(newKeep)]
        let empty = try tree(files: 0)
        model.show(empty, record: record(for: empty), keepPlace: true)
        #expect(model.directory == empty.root)
        #expect(model.selectedItems.isEmpty)
        #expect(model.browser?.backStack.allSatisfy { $0 == empty.root } == true)
        model.closeScan()
        #expect(model.browser == nil)
        #expect(retiring.tree.name(retiring.directory) == "A")
    }
}
