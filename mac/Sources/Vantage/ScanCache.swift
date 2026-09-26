import CryptoKit
import Foundation

/// Saved scans in ~/Library/Caches, so reopening a folder is instant and
/// rescans can start from the saved tree instead of from nothing.
enum ScanCache {
    struct Record: Codable, Identifiable, Equatable {
        var root: String
        var allocated: Bool
        var mark: ChangeJournal.Mark
        var scannedAt: Date
        var totalSize: UInt64
        var fileCount: Int
        // Older records decode this as nil and keep their original blob path.
        var snapshot: UUID? = UUID()

        var id: String { ScanCache.key(root: root, allocated: allocated) }
        var url: URL { URL(fileURLWithPath: root, isDirectory: true) }
    }

    static var directory: URL {
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("dev.cyr1en.Vantage/Scans", isDirectory: true)
    }

    static func key(root: String, allocated: Bool) -> String {
        let digest = SHA256.hash(data: Data("\(root)|\(allocated ? "allocated" : "logical")".utf8))
        return digest.prefix(12).map { String(format: "%02x", $0) }.joined()
    }

    private static let writeLock = NSLock()

    private static func files(for key: String, snapshot: UUID? = nil) -> (data: URL, meta: URL) {
        let suffix = snapshot.map { "." + $0.uuidString } ?? ""
        return (directory.appendingPathComponent("\(key)\(suffix).svx"), directory.appendingPathComponent("\(key).json"))
    }

    static func record(for root: URL, allocated: Bool) -> Record? {
        let meta = files(for: key(root: canonical(root), allocated: allocated)).meta
        return readRecord(at: meta)
    }

    private static func readRecord(at meta: URL) -> Record? {
        guard let data = try? Data(contentsOf: meta) else { return nil }
        return try? JSONDecoder().decode(Record.self, from: data)
    }

    static func load(_ record: Record) throws -> ScanTree {
        let tree = try ScanTree(export: Data(contentsOf: files(for: record.id, snapshot: record.snapshot).data,
                                            options: .mappedIfSafe))
        guard tree.rootURL.path == record.root, tree.allocated == record.allocated,
              tree.totalSize == record.totalSize, tree.fileCount == record.fileCount else { throw ScanTree.FormatError() }
        return tree
    }

    /// Writes the tree and its change-journal mark. Call off the main thread.
    static func save(_ serialized: Data, record: Record) {
        writeLock.lock(); defer { writeLock.unlock() }
        let paths = files(for: record.id, snapshot: record.snapshot)
        let previous = readRecord(at: paths.meta)
        // A slower save from another window must not replace a newer scan.
        if let previous, previous.scannedAt > record.scannedAt { return }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try serialized.write(to: paths.data, options: .atomic)
            // Publish the mark only after its uniquely named tree is complete.
            try JSONEncoder().encode(record).write(to: paths.meta, options: .atomic)
            if let previous, previous.snapshot != record.snapshot {
                try? FileManager.default.removeItem(at: files(for: previous.id, snapshot: previous.snapshot).data)
            }
        } catch {
            if previous?.snapshot != record.snapshot { try? FileManager.default.removeItem(at: paths.data) }
            NSLog("Vantage: could not save scan: %@", error.localizedDescription)
        }
    }

    static func forget(_ record: Record) {
        writeLock.lock(); defer { writeLock.unlock() }
        let paths = files(for: record.id, snapshot: record.snapshot)
        try? FileManager.default.removeItem(at: paths.data)
        if readRecord(at: paths.meta) == record {
            try? FileManager.default.removeItem(at: paths.meta)
        }
    }

    static func recent(limit: Int = 6) -> [Record] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: directory.path) else { return [] }
        let decoder = JSONDecoder()
        return names.filter { $0.hasSuffix(".json") }
            .compactMap { try? decoder.decode(Record.self, from: Data(contentsOf: directory.appendingPathComponent($0))) }
            .filter { fm.fileExists(atPath: $0.root) }
            .sorted { $0.scannedAt > $1.scannedAt }
            .prefix(limit)
            .map { $0 }
    }

    /// The scanner reports real paths; key the cache the same way.
    static func canonical(_ url: URL) -> String {
        guard let resolved = realpath(url.path, nil) else { return url.standardizedFileURL.path }
        defer { free(resolved) }
        return String(cString: resolved)
    }
}
