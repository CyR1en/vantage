import Foundation
import Testing
@testable import Vantage

/// Opt-in: VANTAGE_PERF=/some/large/folder swift test -c release --filter Performance
@Suite struct PerformanceTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["VANTAGE_PERF"] != nil))
    func largeScan() async throws {
        let path = ProcessInfo.processInfo.environment["VANTAGE_PERF"]!
        let root = URL(fileURLWithPath: path)
        let mark = ChangeJournal.mark(for: root)
        var t = Date()
        let (tree, data) = try await Scanner().scanExport(root, allocated: true) { _ in }
        let full = Date().timeIntervalSince(t)

        t = Date()
        let reopened = try ScanTree(export: data)
        let reopen = Date().timeIntervalSince(t)

        let wait = Double(ProcessInfo.processInfo.environment["VANTAGE_PERF_WAIT"] ?? "30") ?? 30
        try await Task.sleep(for: .seconds(wait))

        t = Date()
        let changes = await ChangeJournal.changes(under: root, since: mark)
        let journal = Date().timeIntervalSince(t)
        let result = try await IncrementalScan.refresh(reopened, changes: changes)
        let refresh = Date().timeIntervalSince(t)
        t = Date()
        let saved = result?.tree.serialize()
        let serialize = Date().timeIntervalSince(t)

        print("PERF entries=\(tree.count) fullScan=\(String(format: "%.2f", full))s reopen=\(String(format: "%.2f", reopen))s " +
              "journal=\(String(format: "%.2f", journal))s changedFolders=\(changes.shallow.count)+\(changes.deep.count)deep " +
              "full=\(changes.needsFullScan) refresh=\(String(format: "%.2f", refresh))s " +
              "listed=\(result?.foldersListed ?? -1) subtrees=\(result?.subtreesScanned ?? -1) " +
              "serialize=\(String(format: "%.2f", serialize))s (\((saved?.count ?? 0) >> 20) MiB)")
    }
}
