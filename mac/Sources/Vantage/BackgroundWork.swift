import Foundation

/// Detaches CPU or blocking I/O work while preserving the caller's cancellation.
enum BackgroundWork {
    static func run<Value>(priority: TaskPriority = .userInitiated,
                           _ operation: @escaping @Sendable () async throws -> Value) async throws -> Value {
        let task = Task.detached(priority: priority) {
            try Task.checkCancellation()
            let value = try await operation()
            try Task.checkCancellation()
            return value
        }
        return try await withTaskCancellationHandler {
            try await task.value
        } onCancel: {
            task.cancel()
        }
    }
}
