import AppKit
import Observation
import SwiftUI

/// Detects and requests Full Disk Access. The scanner helper runs as a child of
/// the app, so macOS attributes its file access to Vantage: probing from the app
/// reflects what the helper can read.
enum FullDiskAccess {
    enum Status: Equatable {
        case granted
        case denied
        /// No probe was conclusive (for example, none of the protected folders exist).
        case unknown
    }

    /// Folders that always exist and that only Full Disk Access makes listable.
    /// Their POSIX permissions allow the user to read them, so a refusal comes from
    /// privacy protection rather than ownership.
    static var probes: [String] {
        let library = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library").path
        return [
            "/Library/Application Support/com.apple.TCC",
            library + "/Safari",
            library + "/Mail",
            library + "/Containers/com.apple.stocks",
        ]
    }

    static func check(_ paths: [String] = probes) -> Status {
        for path in paths {
            if let status = probe(path) { return status }
        }
        return .unknown
    }

    /// Lists one entry: success means access, EPERM/EACCES means denied, and a
    /// missing folder says nothing either way.
    static func probe(_ path: String) -> Status? {
        guard let dir = opendir(path) else {
            return errno == EPERM || errno == EACCES ? .denied : nil
        }
        defer { closedir(dir) }
        errno = 0
        if readdir(dir) == nil, errno == EPERM || errno == EACCES { return .denied }
        return .granted
    }

    static func openSettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles") {
            NSWorkspace.shared.open(url)
        }
    }

    /// True when running from Vantage.app. A bare executable (`swift run`) has its
    /// access attributed to whichever app launched it, such as Terminal.
    static var isAppBundle: Bool { Bundle.main.bundleURL.pathExtension == "app" }

    /// Quits and opens the app again once this process has exited, so a new grant
    /// takes effect.
    static func relaunch() {
        guard isAppBundle else { return }
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/bin/sh")
        task.arguments = ["-c", "while /bin/kill -0 \(ProcessInfo.processInfo.processIdentifier) 2>/dev/null; do /bin/sleep 0.2; done; /usr/bin/open \"$0\"",
                          Bundle.main.bundlePath]
        do { try task.run() } catch { return }
        NSApp.terminate(nil)
    }
}

/// State for the Full Disk Access guide window, shared by every browser window.
@MainActor
@Observable
final class AccessGuide {
    static let shared = AccessGuide()
    static let windowID = "access"

    enum Page { case intro, grant, done }

    /// How far the user got, remembered across launches (including a Quit & Reopen).
    enum Progress: String {
        case notStarted = ""
        case skipped
        case waiting
        case done
    }

    private(set) var status = FullDiskAccess.check()
    var page: Page = .intro
    /// Set once System Settings has been opened from the guide.
    var openedSettings = false
    var isTerminating = false
    private var checkedAtLaunch = false

    var progress: Progress {
        get { Progress(rawValue: UserDefaults.standard.string(forKey: Preferences.accessGuide) ?? "") ?? .notStarted }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: Preferences.accessGuide) }
    }

    /// The page to open at launch, or nil to stay out of the way. Someone who
    /// skipped the guide isn't asked again; the welcome screen offers it instead.
    nonisolated static func launchPage(progress: Progress, status: FullDiskAccess.Status) -> Page? {
        switch (status, progress) {
        case (.granted, .waiting): .done
        case (.denied, .notStarted): .intro
        case (.denied, .waiting): .grant
        default: nil
        }
    }

    /// Opens the guide on the first launch, or resumes it after a Quit & Reopen.
    func showAtLaunchIfNeeded(_ openWindow: OpenWindowAction) {
        guard !checkedAtLaunch else { return }
        checkedAtLaunch = true
        refresh()
        if let page = Self.launchPage(progress: progress, status: status) {
            self.page = page
            openWindow(id: Self.windowID)
        } else if status == .granted, progress != .done {
            progress = .done
        }
    }

    /// Opens the guide from a menu or a "Turn On Full Disk Access" link.
    func show(_ openWindow: OpenWindowAction) {
        refresh()
        page = status == .granted ? .done : .grant
        openWindow(id: Self.windowID)
    }

    func refresh() {
        let current = FullDiskAccess.check()
        guard current != status else { return }
        status = current
        if current == .granted, page == .grant {
            progress = .done
            withAnimation(.smooth) { page = .done }
            NSApp.activate()
        }
    }

    func begin() {
        progress = .waiting
        withAnimation(.smooth) { page = .grant }
    }

    func relaunch() {
        progress = .waiting
        isTerminating = true
        FullDiskAccess.relaunch()
    }

    /// Closing the window, however it happens, ends the guide: without access it
    /// counts as skipping. Quitting to apply a new grant keeps it waiting instead.
    func windowClosed() {
        openedSettings = false
        guard !isTerminating else { return }
        progress = status == .granted ? .done : .skipped
    }
}
