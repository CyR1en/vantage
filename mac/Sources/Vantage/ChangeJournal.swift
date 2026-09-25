import CoreServices
import Foundation

/// Reads macOS's file-system event history (FSEvents) to learn which folders
/// changed since a scan, so a rescan only has to revisit those folders.
enum ChangeJournal {
    struct Changes {
        /// Folders whose immediate contents changed.
        var shallow: Set<String> = []
        /// Folders whose whole subtree must be rescanned (coalesced or dropped events).
        var deep: Set<String> = []
        /// The history can't be trusted; do a full scan.
        var needsFullScan = false
    }

    /// A marker taken just before a scan starts. Changes made during the scan
    /// are replayed on the next refresh, so nothing is missed.
    struct Mark: Codable, Equatable {
        var eventID: UInt64
        var volumeUUID: String?
    }

    static func mark(for url: URL) -> Mark {
        Mark(eventID: FSEventsGetCurrentEventId(), volumeUUID: volumeUUID(for: url))
    }

    static func volumeUUID(for url: URL) -> String? {
        var st = stat()
        guard lstat(url.path, &st) == 0, let uuid = FSEventsCopyUUIDForDevice(st.st_dev) else { return nil }
        return CFUUIDCreateString(nil, uuid) as String
    }

    /// Replays events under `root` since `mark`. Returns within `timeout`, or asks for a full scan.
    static func changes(under root: URL, since mark: Mark, timeout: TimeInterval = 20) async -> Changes {
        guard let uuid = mark.volumeUUID, uuid == volumeUUID(for: root) else {
            return Changes(needsFullScan: true)   // the volume's event database was reset
        }
        let collector = Collector(root: root.path)
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                collector.start(since: mark.eventID, timeout: timeout) { continuation.resume(returning: $0) }
            }
        } onCancel: {
            collector.cancel()
        }
    }

    private final class Collector: @unchecked Sendable {
        let root: String
        private let queue = DispatchQueue(label: "Vantage.ChangeJournal")
        private var stream: FSEventStreamRef?
        private var result = Changes()
        private var done: ((Changes) -> Void)?
        private var cancelled = false
        private var timeoutWork: DispatchWorkItem?

        init(root: String) { self.root = root }

        func start(since eventID: UInt64, timeout: TimeInterval, done: @escaping (Changes) -> Void) {
            queue.async { self.startOnQueue(since: eventID, timeout: timeout, done: done) }
        }

        func cancel() {
            queue.async {
                self.cancelled = true
                self.finish(Changes(needsFullScan: true))
            }
        }

        private func startOnQueue(since eventID: UInt64, timeout: TimeInterval, done: @escaping (Changes) -> Void) {
            guard !cancelled else { done(Changes(needsFullScan: true)); return }
            self.done = done
            var context = FSEventStreamContext(version: 0, info: Unmanaged.passUnretained(self).toOpaque(),
                                               retain: nil, release: nil, copyDescription: nil)
            let callback: FSEventStreamCallback = { _, info, count, paths, flags, _ in
                let collector = Unmanaged<Collector>.fromOpaque(info!).takeUnretainedValue()
                let list = Unmanaged<CFArray>.fromOpaque(paths).takeUnretainedValue() as! [String]
                collector.receive(list, Array(UnsafeBufferPointer(start: flags, count: count)))
            }
            guard let stream = FSEventStreamCreate(
                nil, callback, &context, [root] as CFArray, eventID, 0,
                FSEventStreamCreateFlags(kFSEventStreamCreateFlagUseCFTypes | kFSEventStreamCreateFlagNoDefer))
            else {
                finish(Changes(needsFullScan: true)); return
            }
            self.stream = stream
            FSEventStreamSetDispatchQueue(stream, queue)
            guard FSEventStreamStart(stream) else { finish(Changes(needsFullScan: true)); return }
            let timeoutWork = DispatchWorkItem { [weak self] in
                self?.finish(Changes(needsFullScan: true))
            }
            self.timeoutWork = timeoutWork
            queue.asyncAfter(deadline: .now() + timeout, execute: timeoutWork)
        }

        private func receive(_ paths: [String], _ flags: [FSEventStreamEventFlags]) {
            guard done != nil else { return }
            for (path, raw) in zip(paths, flags) {
                let flag = Int(raw)
                if flag & kFSEventStreamEventFlagHistoryDone != 0 { finish(result); return }
                if flag & (kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped |
                           kFSEventStreamEventFlagEventIdsWrapped | kFSEventStreamEventFlagRootChanged |
                           kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount) != 0 {
                    result.needsFullScan = true
                    continue
                }
                let folder = Self.normalize(path)
                if flag & kFSEventStreamEventFlagMustScanSubDirs != 0 {
                    result.deep.insert(folder)
                } else {
                    result.shallow.insert(folder)
                }
            }
        }

        private func finish(_ changes: Changes) {
            guard let done else { return }   // already finished (history done or timed out)
            self.done = nil
            timeoutWork?.cancel()
            timeoutWork = nil
            // This usually runs inside the stream's callback; releasing the stream
            // there would free it while FSEvents is still using it. Tear down afterwards.
            if let stream {
                self.stream = nil
                queue.async {
                    FSEventStreamStop(stream)
                    FSEventStreamInvalidate(stream)
                    FSEventStreamRelease(stream)
                    _ = self   // keep the callback's context alive until the stream is gone
                }
            }
            DispatchQueue.global().async { done(changes) }
        }

        static func normalize(_ path: String) -> String {
            var p = path
            while p.count > 1 && p.hasSuffix("/") { p.removeLast() }
            return p
        }
    }
}
