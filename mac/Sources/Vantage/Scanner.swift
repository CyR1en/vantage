import Foundation

/// Runs the bundled `vantage` executable in `--export` mode. Scanning in a
/// separate process keeps the app responsive and unaffected by scanner limits.
final class Scanner: @unchecked Sendable {
    struct Progress: Sendable {
        var entries: UInt64
        var elapsed: TimeInterval
    }

    enum Failure: LocalizedError {
        case missingHelper
        case scanner(String)
        case cancelled

        var errorDescription: String? {
            switch self {
            case .missingHelper: "Vantage’s scanning component is missing. Reinstall the app."
            case .scanner(let message): message
            case .cancelled: "The scan was cancelled."
            }
        }
    }

    private let lock = NSLock()
    private var process: Process?
    private var cancelled = false
    private let helperOverride: URL?

    init(helperURL: URL? = nil) { helperOverride = helperURL }

    static func helperURL() -> URL? {
        let bundled = Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/vantage")
        if FileManager.default.isExecutableFile(atPath: bundled.path) { return bundled }
        if let override = ProcessInfo.processInfo.environment["VANTAGE_HELPER"] {
            return URL(fileURLWithPath: override)
        }
        // Development fallback, including test runners hosted outside the repo.
        let exe = Bundle.main.executableURL ?? URL(fileURLWithPath: CommandLine.arguments[0])
        let workingDirectory = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
        for var dir in [exe.deletingLastPathComponent(), workingDirectory] {
            while dir.path != "/" {
                let candidate = dir.appendingPathComponent("build/vantage")
                if FileManager.default.isExecutableFile(atPath: candidate.path) { return candidate }
                dir.deleteLastPathComponent()
            }
        }
        return nil
    }

    func cancel() {
        lock.withLock {
            cancelled = true
            if let process, process.isRunning { process.terminate() }
        }
    }

    func scan(_ url: URL, allocated: Bool, progress: @escaping @Sendable (Progress) -> Void) async throws -> ScanTree {
        try await scanExport(url, allocated: allocated, progress: progress).tree
    }

    /// Scans and also returns the raw export, which can be saved as-is.
    func scanExport(_ url: URL, allocated: Bool,
                    progress: @escaping @Sendable (Progress) -> Void) async throws -> (tree: ScanTree, data: Data) {
        try await withTaskCancellationHandler {
            try await performScanExport(url, allocated: allocated, progress: progress)
        } onCancel: {
            self.cancel()
        }
    }

    private func performScanExport(_ url: URL, allocated: Bool,
                                   progress: @escaping @Sendable (Progress) -> Void) async throws -> (tree: ScanTree, data: Data) {
        try Task.checkCancellation()
        guard let helper = helperOverride ?? Self.helperURL() else { throw Failure.missingHelper }
        let physical = ProcessInfo.processInfo.physicalMemory
        let memoryLimit = max(UInt64(1) << 30, min(physical / 2, UInt64(16) << 30))

        let task = Process()
        task.executableURL = helper
        var arguments = ["--export", "--timeout", "0", "--memory-limit", "\(memoryLimit)"]
        if allocated { arguments.append("--allocated") }
        arguments += ["--", url.path]
        task.arguments = arguments
        let out = Pipe(), err = Pipe()
        task.standardOutput = out
        task.standardError = err
        task.standardInput = FileHandle.nullDevice

        defer {
            for handle in [out.fileHandleForReading, out.fileHandleForWriting,
                           err.fileHandleForReading, err.fileHandleForWriting] { try? handle.close() }
        }
        let messages = MessageBuffer()
        let receive: @Sendable (Data) -> Void = { chunk in
            for line in messages.append(chunk) {
                let parts = line.split(separator: " ")
                if parts.count == 3, parts[0] == "progress",
                   let n = UInt64(parts[1]), let ms = UInt64(parts[2]) {
                    progress(Progress(entries: n, elapsed: TimeInterval(ms) / 1000))
                } else if !line.isEmpty {
                    messages.note(line)
                }
            }
        }

        let data: Data = try await withCheckedThrowingContinuation { continuation in
            DispatchQueue.global(qos: .userInitiated).async {
                let stderrDone = DispatchGroup()
                defer { self.lock.withLock { if self.process === task { self.process = nil } } }
                do {
                    // Publication and launch are atomic with respect to cancel().
                    // A cancellation can no longer land between them and be lost.
                    try self.lock.withLock {
                        guard !self.cancelled else { throw Failure.cancelled }
                        try task.run()
                        self.process = task
                    }
                    try? out.fileHandleForWriting.close()
                    try? err.fileHandleForWriting.close()
                    stderrDone.enter()
                    DispatchQueue.global(qos: .utility).async {
                        defer { stderrDone.leave() }
                        while let chunk = try? err.fileHandleForReading.read(upToCount: 8192), !chunk.isEmpty {
                            receive(chunk)
                        }
                    }
                    let data = try out.fileHandleForReading.readToEnd() ?? Data()
                    task.waitUntilExit()
                    stderrDone.wait()
                    continuation.resume(returning: data)
                } catch {
                    if task.isRunning { task.terminate(); task.waitUntilExit() }
                    stderrDone.wait()
                    continuation.resume(throwing: error)
                }
            }
        }
        let wasCancelled = lock.withLock { cancelled }
        if wasCancelled { throw Failure.cancelled }
        try Task.checkCancellation()

        guard data.starts(with: Array("SVX1".utf8)) else {
            var message = messages.lastMessage ?? "The scan ended unexpectedly (status \(task.terminationStatus))."
            if message.hasPrefix("vantage: ") { message.removeFirst("vantage: ".count) }
            throw Failure.scanner(message.prefix(1).uppercased() + message.dropFirst())
        }
        let tree = try await BackgroundWork.run {
            try ScanTree(export: data)
        }
        if lock.withLock({ cancelled }) { throw Failure.cancelled }
        return (tree, data)
    }
}

private final class MessageBuffer: @unchecked Sendable {
    private let lock = NSLock()
    private var pending = Data()
    private var last: String?

    func append(_ chunk: Data) -> [String] {
        lock.lock(); defer { lock.unlock() }
        pending.append(chunk)
        var lines: [String] = []
        while let newline = pending.firstIndex(of: 0x0A) {
            lines.append(String(decoding: pending[pending.startIndex..<newline], as: UTF8.self))
            pending.removeSubrange(pending.startIndex...newline)
        }
        if pending.count > 65_536 { pending = Data(pending.suffix(65_536)) }
        return lines
    }

    func note(_ line: String) {
        lock.lock(); last = line; lock.unlock()
    }

    var lastMessage: String? {
        lock.lock(); defer { lock.unlock() }
        if !pending.isEmpty { return String(decoding: pending, as: UTF8.self) }
        return last
    }
}
