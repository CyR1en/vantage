import Observation

/// Node indices belong to one scan. Retiring views keep this state with their
/// tree while a refresh installs a replacement browser.
@MainActor
@Observable
final class BrowserState {
    let tree: ScanTree
    var directory: Int
    var selection: Set<Int> = []
    var backStack: [Int] = []
    var forwardStack: [Int] = []
    var revision = 0

    init(tree: ScanTree) {
        self.tree = tree
        directory = tree.root
    }

    var selectedItems: [Int] {
        selection.filter { $0 >= 0 && $0 < tree.count && !tree.isRemoved($0) }.sorted()
    }
}
