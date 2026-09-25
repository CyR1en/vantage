import Foundation
import Testing
@testable import Vantage

@Suite struct ParsePerfTests {
    @Test(.enabled(if: ProcessInfo.processInfo.environment["VANTAGE_EXPORT"] != nil))
    func parse() throws {
        let data = try Data(contentsOf: URL(fileURLWithPath: ProcessInfo.processInfo.environment["VANTAGE_EXPORT"]!))
        let t = Date()
        let tree = try ScanTree(export: data)
        print("PARSE entries=\(tree.count) seconds=\(Date().timeIntervalSince(t))")
    }
}
