import AppKit
import SwiftUI

struct ContentView: View {
    @State private var model = AppModel()
    @Environment(\.openWindow) private var openWindow

    init() {}

    init(model: AppModel) {
        _model = State(initialValue: model)
    }

    @State private var windowWidth: CGFloat = 1180

    var body: some View {
        AppShell {
            switch model.phase {
            case .welcome:
                WelcomeView()
            case .scanning(let url):
                ScanningView(url: url)
            case .failed(let url, let message):
                FailedView(url: url, message: message)
            case .browsing:
                if let browser = model.browser {
                    BrowserView()
                        .environment(browser)
                        .id(browser.tree.id)
                }
            }
        }
        .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { windowWidth = $0 }
        .toolbar {
            ToolbarItem(placement: .navigation) {
                NavigationButtons()
            }
            ToolbarItem(placement: .navigation) {
                RailToggle()
            }
            ToolbarSpacer(.flexible)
            ToolbarItem(placement: .primaryAction) {
                LocationBar(width: Chrome.locationBarWidth(window: windowWidth))
            }
            ToolbarSpacer(.fixed, placement: .primaryAction)
            ToolbarItem(placement: .primaryAction) {
                SearchButton()
            }
        }
        .toolbar(removing: .title)
        .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
        .frame(minWidth: 820, minHeight: 520)
        .environment(model)
        .focusedSceneValue(\.appModel, model)
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first(where: \.hasDirectoryPath) ?? urls.first.map({ $0.deletingLastPathComponent() }) else { return false }
            model.scan(url)
            return true
        }
        .onAppear {
            takePendingFolder()
            AccessGuide.shared.showAtLaunchIfNeeded(openWindow)
        }
        .onReceive(NotificationCenter.default.publisher(for: .scanFolderRequested)) { _ in takePendingFolder() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.willTerminateNotification)) { _ in
            model.cancelScan()
        }
        .overlay(alignment: .bottom) {
            if let toast = model.toast {
                Label(toast, icon: .done, weight: .fill, size: 16)
                    .font(.callout.weight(.medium))
                    .padding(.horizontal, 16)
                    .padding(.vertical, 10)
                    .glassEffect(.regular, in: .capsule)
                    .padding(.bottom, 28)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        .alert(deletionTitle, isPresented: deletionShown, presenting: model.pendingDeletion) { request in
            Button(request.permanently ? "Delete" : "Move to Trash", role: request.permanently ? .destructive : nil) {
                model.performDeletion(request)
            }
            .keyboardShortcut(.defaultAction)
            Button("Cancel", role: .cancel) { model.pendingDeletion = nil }
        } message: { request in
            if request.permanently {
                Text("This will free \(Format.bytes(request.bytes)). The items will be deleted immediately. You can’t undo this action.")
            } else {
                Text("This will free \(Format.bytes(request.bytes)) once the Trash is emptied.")
            }
        }
        .alert(model.alert?.title ?? "", isPresented: alertShown, presenting: model.alert) { _ in
            Button("OK") { model.alert = nil }
        } message: { alert in
            Text(alert.message)
        }
    }

    private func takePendingFolder() {
        guard let url = AppDelegate.pendingFolder else { return }
        AppDelegate.pendingFolder = nil
        model.scan(url)
    }

    private var deletionTitle: String {
        guard let request = model.pendingDeletion else { return "" }
        return request.permanently
            ? "Are you sure you want to delete \(request.title) immediately?"
            : "Move \(request.title) to the Trash?"
    }

    private var deletionShown: Binding<Bool> {
        Binding(get: { model.pendingDeletion != nil }, set: { if !$0 { model.pendingDeletion = nil } })
    }

    private var alertShown: Binding<Bool> {
        Binding(get: { model.alert != nil }, set: { if !$0 { model.alert = nil } })
    }
}

// MARK: - Welcome

struct WelcomeView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openWindow) private var openWindow
    @State private var recents: [ScanCache.Record] = []

    private struct Place: Identifiable {
        let id = UUID()
        let title: String
        let icon: Icon
        let url: URL
    }

    private var places: [Place] {
        let fm = FileManager.default
        let home = fm.homeDirectoryForCurrentUser
        func dir(_ d: FileManager.SearchPathDirectory) -> URL? { fm.urls(for: d, in: .userDomainMask).first }
        var list = [Place(title: "Home", icon: .home, url: home)]
        if let u = dir(.desktopDirectory) { list.append(Place(title: "Desktop", icon: .desktop, url: u)) }
        if let u = dir(.documentDirectory) { list.append(Place(title: "Documents", icon: .documents, url: u)) }
        if let u = dir(.downloadsDirectory) { list.append(Place(title: "Downloads", icon: .downloads, url: u)) }
        list.append(Place(title: "Applications", icon: .applications, url: URL(fileURLWithPath: "/Applications")))
        let disk = URL(fileURLWithPath: "/")
        let diskName = (try? URL(fileURLWithPath: "/").resourceValues(forKeys: [.volumeNameKey]).volumeName) ?? "Macintosh HD"
        list.append(Place(title: diskName, icon: .disk, url: disk))
        return list
    }

    var body: some View {
        GeometryReader { proxy in
            ScrollView {
                content
                    .frame(maxWidth: .infinity, minHeight: proxy.size.height)
            }
            .scrollBounceBehavior(.basedOnSize)
        }
        .background(WelcomeBackground.wash)
        .task { recents = ScanCache.recent(limit: 4) }
        .navigationTitle("Vantage")
    }

    private var content: some View {
        VStack(spacing: 28) {
            VStack(spacing: 14) {
                ZStack {
                    Circle()
                        .fill(.tint.opacity(0.12))
                        .frame(width: 112, height: 112)
                    IconImage(.summary, weight: .duotone, size: 60)
                        .foregroundStyle(Color.accentColor.gradient)
                }
                .glassEffect(.regular, in: .circle)
                Text("See What’s Taking Up Space")
                    .font(.largeTitle.weight(.bold))
                Text("Choose a folder or disk. Vantage finds the biggest files and folders so you can clean them up.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 460)
            }

            Button {
                model.chooseFolder()
            } label: {
                Label("Choose Folder…", icon: .chooseFolder, weight: .bold, size: 18)
                    .padding(.horizontal, 8)
            }
            .buttonStyle(.glassProminent)
            .controlSize(.extraLarge)
            .keyboardShortcut(.defaultAction)

            if !recents.isEmpty {
                VStack(spacing: 10) {
                    Text("Recent scans")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                    VStack(spacing: 0) {
                        ForEach(recents) { record in
                            RecentScanRow(record: record) { model.scan(record.url) } forget: {
                                ScanCache.forget(record)
                                recents = ScanCache.recent()
                            }
                            if record != recents.last { Divider().padding(.leading, 44) }
                        }
                    }
                    .padding(.vertical, 4)
                    .frame(width: 374)
                    .glassEffect(.regular, in: .rect(cornerRadius: 16))
                }
            }

            VStack(spacing: 10) {
                Text("Or scan a common place")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                GlassEffectContainer(spacing: 10) {
                    LazyVGrid(columns: Array(repeating: GridItem(.fixed(118), spacing: 10), count: 3), spacing: 10) {
                        ForEach(places) { place in
                            Button {
                                model.scan(place.url)
                            } label: {
                                VStack(spacing: 6) {
                                    IconImage(place.icon, weight: .duotone, size: 26)
                                        .foregroundStyle(.tint)
                                    Text(place.title)
                                        .font(.callout.weight(.medium))
                                        .lineLimit(1)
                                }
                                .frame(width: 102, height: 64)
                            }
                            .buttonStyle(.glass)
                            .help(place.url.path)
                        }
                    }
                }
            }

            if AccessGuide.shared.status == .denied {
                Button { AccessGuide.shared.show(openWindow) } label: {
                    HStack(spacing: 8) {
                        IconImage(.protected, weight: .fill, size: 17)
                            .foregroundStyle(.orange)
                        Text("Full Disk Access is off, so protected folders will be skipped.")
                            .foregroundStyle(.secondary)
                        Text("Turn On…")
                            .fontWeight(.medium)
                            .foregroundStyle(.tint)
                    }
                    .font(.callout)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .contentShape(.capsule)
                }
                .buttonStyle(.plain)
                .glassEffect(.regular.interactive(), in: .capsule)
            }

            Text("You can also drop a folder onto this window.")
                .font(.footnote)
                .foregroundStyle(.tertiary)
        }
        .padding(40)
    }
}

private struct RecentScanRow: View {
    let record: ScanCache.Record
    let open: () -> Void
    let forget: () -> Void

    var body: some View {
        Button(action: open) {
            HStack(spacing: 10) {
                Image(nsImage: NSWorkspace.shared.icon(forFile: record.root))
                    .resizable()
                    .frame(width: 24, height: 24)
                VStack(alignment: .leading, spacing: 1) {
                    Text(FileManager.default.displayName(atPath: record.root))
                        .font(.callout.weight(.medium))
                        .lineLimit(1)
                    Text("\(Format.bytes(record.totalSize)) · scanned \(record.scannedAt, format: .relative(presentation: .named))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer()
                IconImage(.forward, weight: .bold, size: 11)
                    .foregroundStyle(.tertiary)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 7)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .help(record.root)
        .contextMenu {
            Button("Open", action: open)
            Button("Forget This Scan", role: .destructive, action: forget)
        }
    }
}

struct WelcomeBackground: View {
    static var wash: LinearGradient {
        LinearGradient(colors: [.accentColor.opacity(0.10), .clear, FileCategory.image.color.opacity(0.06)],
                       startPoint: .topLeading, endPoint: .bottomTrailing)
    }

    var body: some View {
        ZStack {
            Color(nsColor: .windowBackgroundColor)
            Self.wash
        }
        .ignoresSafeArea()
    }
}

// MARK: - Scanning

struct ScanningView: View {
    @Environment(AppModel.self) private var model
    let url: URL

    var body: some View {
        VStack(spacing: 22) {
            ProgressView()
                .controlSize(.extraLarge)
            VStack(spacing: 6) {
                Text("\(model.isOpeningSaved ? "Opening" : "Scanning") “\(url.lastPathComponent.isEmpty ? url.path : url.lastPathComponent)”")
                    .font(.title2.weight(.semibold))
                Text(detail)
                    .font(.body)
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
                    .animation(.snappy, value: model.scanProgress.entries)
            }
            Button("Cancel", role: .cancel) { model.cancelScan() }
                .buttonStyle(.glass)
                .controlSize(.large)
                .keyboardShortcut(.cancelAction)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .navigationTitle("Vantage")
    }

    private var detail: String {
        let p = model.scanProgress
        if model.isOpeningSaved { return "Loading your last scan…" }
        let found = p.entries > 0 ? "\(Format.count(Int(p.entries))) items found" : "Looking for files…"
        return p.elapsed >= 1 ? "\(found) · \(Int(p.elapsed)) s" : found
    }
}

// MARK: - Failed

struct FailedView: View {
    @Environment(AppModel.self) private var model
    @Environment(\.openWindow) private var openWindow
    let url: URL
    let message: String

    var body: some View {
        ContentUnavailableView {
            Label("Couldn’t Scan “\(url.lastPathComponent)”", icon: .warning, weight: .duotone, size: 48)
        } description: {
            Text(message)
        } actions: {
            HStack {
                Button("Choose Another Folder…") { model.chooseFolder() }
                    .buttonStyle(.glass)
                Button("Try Again") { model.scan(url) }
                    .buttonStyle(.glassProminent)
            }
            if AccessGuide.shared.status != .granted,
               message.localizedCaseInsensitiveContains("permission") || message.localizedCaseInsensitiveContains("not permitted") {
                Button("Turn On Full Disk Access…") { AccessGuide.shared.show(openWindow) }
                    .buttonStyle(.link)
            }
        }
        .navigationTitle("Vantage")
    }
}
