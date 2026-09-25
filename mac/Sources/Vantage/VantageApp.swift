import AppKit
import SwiftUI

@main
struct VantageApp: App {
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate

    var body: some Scene {
        WindowGroup("Vantage", id: "browser") {
            ContentView()
        }
        .defaultSize(width: 1180, height: 760)
        .windowToolbarStyle(.unified)
        .commands { VantageCommands() }

        Settings {
            SettingsView()
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    /// A folder handed to the app (Dock drop or `open -a Vantage DIR`) awaiting a window.
    static var pendingFolder: URL?
    static var openWindow: OpenWindowAction?

    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first(where: { (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true })
        else { return }
        Self.pendingFolder = url
        // An existing window takes the folder; otherwise open one to show it.
        NotificationCenter.default.post(name: .scanFolderRequested, object: nil)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            if Self.pendingFolder == url { Self.openWindow?(id: "browser") }
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

extension Notification.Name {
    static let scanFolderRequested = Notification.Name("VantageScanFolderRequested")
}

extension FocusedValues {
    @Entry var appModel: AppModel?
}

struct VantageCommands: Commands {
    @FocusedValue(\.appModel) private var model
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        let _ = { AppDelegate.openWindow = openWindow }()
        CommandGroup(replacing: .newItem) {
            Button("New Window") { openWindow(id: "browser") }
                .keyboardShortcut("n")
            Button("Scan Folder…") { model?.chooseFolder() }
                .keyboardShortcut("o")
                .disabled(model == nil)
            Button("Rescan") { model?.rescan() }
                .keyboardShortcut("r")
                .disabled(model?.scannedURL == nil || model?.isUpdating == true)
            Button("Full Rescan") { model?.fullScan() }
                .keyboardShortcut("r", modifiers: [.command, .option])
                .disabled(model?.scannedURL == nil)
            Button("Close Scan") { model?.closeScan() }
                .keyboardShortcut("w", modifiers: [.command, .shift])
                .disabled(model?.tree == nil)
        }

        CommandGroup(after: .pasteboard) {
            let selected = model?.selectedItems ?? []
            Divider()
            Button("Copy Path") { model?.copyPaths(selected) }
                .keyboardShortcut("c", modifiers: [.command, .option])
                .disabled(selected.isEmpty)
        }

        CommandMenu("Item") {
            let selected = model?.selectedItems ?? []
            Button("Open") { if let item = selected.first { model?.open(item) } }
                .keyboardShortcut(.downArrow)
                .disabled(selected.count != 1)
            Button("Quick Look") { model?.quickLook() }
                .keyboardShortcut("y")
                .disabled(selected.isEmpty)
            Button("Show in Finder") { model?.reveal(selected) }
                .keyboardShortcut("r", modifiers: [.command, .shift])
                .disabled(selected.isEmpty)
            Button("Open with Default App") { model?.openWithDefaultApp(selected) }
                .keyboardShortcut("o", modifiers: [.command, .shift])
                .disabled(selected.isEmpty)
            Divider()
            Button("Move to Trash") { model?.requestDeletion(selected) }
                .keyboardShortcut(.delete)
                .disabled(selected.isEmpty)
            Button("Delete Immediately…") { model?.requestDeletion(selected, permanently: true) }
                .keyboardShortcut(.delete, modifiers: [.command, .option])
                .disabled(selected.isEmpty)
        }

        CommandMenu("Go") {
            Button("Back") { model?.goBack() }
                .keyboardShortcut("[")
                .disabled(!(model?.canGoBack ?? false))
            Button("Forward") { model?.goForward() }
                .keyboardShortcut("]")
                .disabled(!(model?.canGoForward ?? false))
            Button("Enclosing Folder") { model?.goUp() }
                .keyboardShortcut(.upArrow)
                .disabled(!(model?.canGoUp ?? false))
            Button("Scanned Folder") { model?.goToRoot() }
                .keyboardShortcut("h", modifiers: [.command, .shift])
                .disabled(model?.tree == nil)
            Divider()
            Button("Folders") { model?.sidebar = .folders }
                .keyboardShortcut("1")
                .disabled(model?.tree == nil)
            Button("Largest Files") { model?.sidebar = .largestFiles }
                .keyboardShortcut("2")
                .disabled(model?.tree == nil)
        }
    }
}

struct SettingsView: View {
    @AppStorage(Preferences.apparentSize) private var apparentSize = false
    @AppStorage(Preferences.confirmTrash) private var confirmTrash = true

    var body: some View {
        Form {
            Section {
                Picker("Measure sizes by", selection: $apparentSize) {
                    Text("Actual disk usage").tag(false)
                    Text("Apparent file size").tag(true)
                }
                .pickerStyle(.radioGroup)
                Text("Actual disk usage counts only the space files really occupy, so sparse virtual disks (OrbStack, Docker, UTM) and compressed files aren’t overstated. Apparent file size is the length Finder reports. Takes effect on the next scan.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Section {
                Toggle("Ask before moving items to the Trash", isOn: $confirmTrash)
            }
            Section {
                LabeledContent("Full Disk Access") {
                    Button("Open Privacy Settings…") { AppModel.openFullDiskAccessSettings() }
                }
                Text("To measure protected folders such as Mail or Safari data, add Vantage under Privacy & Security › Full Disk Access.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .frame(width: 460)
        .fixedSize(horizontal: false, vertical: true)
    }
}
