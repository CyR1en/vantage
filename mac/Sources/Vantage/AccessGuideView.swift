import AppKit
import SwiftUI

/// Walks the user through granting Full Disk Access in System Settings and
/// notices the grant as soon as it takes effect.
struct AccessGuideView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var window: NSWindow?
    private let guide = AccessGuide.shared

    var body: some View {
        Group {
            switch guide.page {
            case .intro: intro
            case .grant: grant
            case .done: done
            }
        }
        .transition(.blurReplace)
        .padding(.horizontal, 36)
        .padding(.top, 40)
        .padding(.bottom, 28)
        .frame(width: 540)
        .fixedSize(horizontal: false, vertical: true)
        .background(WelcomeBackground())
        .background(WindowReader { window = $0 })
        .task(id: guide.page) {
            if guide.page == .done { window?.level = .normal }
            // Poll while waiting: System Settings doesn't tell the app about a grant.
            while guide.page == .grant, !Task.isCancelled {
                guide.refresh()
                try? await Task.sleep(for: .seconds(1))
            }
        }
        .onDisappear { guide.windowClosed() }
    }

    // MARK: Intro

    private var intro: some View {
        VStack(spacing: 24) {
            VStack(spacing: 14) {
                Image(nsImage: NSApp.applicationIconImage)
                    .resizable()
                    .frame(width: 96, height: 96)
                    .overlay(alignment: .bottomTrailing) {
                        IconImage(.protected, weight: .fill, size: 22)
                            .foregroundStyle(.white)
                            .frame(width: 40, height: 40)
                            .glassEffect(.regular.tint(.accentColor), in: .circle)
                            .offset(x: 6, y: 6)
                    }
                Text("Let Vantage See Everything")
                    .font(.largeTitle.weight(.bold))
                Text("macOS hides some folders from apps until you allow Full Disk Access. Without it, Vantage has to skip them, and your totals come up short.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }

            VStack(alignment: .leading, spacing: 16) {
                Benefit(icon: .disks, title: "Complete totals",
                        detail: "Mail, Messages, Safari, and other apps’ data are included in every scan.")
                Benefit(icon: .privacy, title: "Private by design",
                        detail: "Scans read names and sizes, not file contents, and nothing leaves your Mac.")
                Benefit(icon: .toggle, title: "Always your choice",
                        detail: "Turn it off at any time in System Settings.")
            }
            .padding(18)
            .frame(maxWidth: .infinity, alignment: .leading)
            .glassEffect(.regular, in: .rect(cornerRadius: 18))

            HStack {
                Button("Not Now") { dismiss() }
                    .buttonStyle(.glass)
                    .keyboardShortcut(.cancelAction)
                Spacer()
                Button("Continue") { guide.begin() }
                    .buttonStyle(.glassProminent)
                    .keyboardShortcut(.defaultAction)
            }
            .controlSize(.large)
        }
    }

    // MARK: Grant

    private var grant: some View {
        VStack(spacing: 20) {
            VStack(spacing: 8) {
                Text("Turn On Full Disk Access")
                    .font(.title.weight(.bold))
                Text("Keep this window open. It updates as soon as Vantage has access.")
                    .font(.body)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }

            VStack(spacing: 0) {
                Step(number: 1, done: guide.openedSettings, title: "Open Privacy & Security",
                     detail: "System Settings opens to the Full Disk Access list.") {
                    Button(guide.openedSettings ? "Open Again" : "Open Settings") { openSettings() }
                        .buttonStyle(.glassProminent)
                        .tint(guide.openedSettings ? .secondary : .accentColor)
                }
                Divider().padding(.leading, 54)
                if FullDiskAccess.isAppBundle {
                    Step(number: 2, title: "Add Vantage",
                         detail: "Drag this icon into the list, or click + and choose Vantage.") {
                        AppDragTile()
                    }
                } else {
                    Step(number: 2, title: "Add the app that runs Vantage",
                         detail: "Vantage is running outside its app bundle, so macOS checks the app that launched it, such as Terminal.") {
                        EmptyView()
                    }
                }
                Divider().padding(.leading, 54)
                Step(number: 3, title: "Switch it on",
                     detail: "If macOS offers to Quit & Reopen Vantage, choose it.") {
                    IconImage(.toggle, weight: .duotone, size: 28)
                        .foregroundStyle(.secondary)
                        .frame(width: 64)
                }
            }
            .glassEffect(.regular, in: .rect(cornerRadius: 18))

            waiting

            HStack {
                Button("Not Now") { dismiss() }
                    .buttonStyle(.glass)
                    .keyboardShortcut(.cancelAction)
                Spacer()
                if guide.openedSettings && FullDiskAccess.isAppBundle {
                    Button("Quit & Reopen") { guide.relaunch() }
                        .buttonStyle(.glass)
                        .help("Reopen Vantage so a new Full Disk Access setting takes effect")
                }
                if guide.status == .unknown {
                    Button("Done") { dismiss() }
                        .buttonStyle(.glassProminent)
                        .keyboardShortcut(.defaultAction)
                }
            }
            .controlSize(.large)
        }
    }

    @ViewBuilder private var waiting: some View {
        if guide.status == .unknown {
            Label("Vantage can’t confirm access on this Mac. Choose Done once it’s switched on.",
                  icon: .help, size: 15)
                .font(.callout)
                .foregroundStyle(.secondary)
        } else {
            VStack(spacing: 4) {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Waiting for Full Disk Access…")
                        .font(.callout.weight(.medium))
                }
                if guide.openedSettings {
                    Text("Already switched on? Vantage may need to reopen to notice.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    // MARK: Done

    private var done: some View {
        VStack(spacing: 22) {
            IconImage(.verified, weight: .fill, size: 80)
                .foregroundStyle(Color.green.gradient)
                .padding(18)
                .glassEffect(.regular, in: .circle)
            VStack(spacing: 8) {
                Text("You’re All Set")
                    .font(.largeTitle.weight(.bold))
                Text("Vantage has Full Disk Access, so scans include protected folders such as Mail, Messages, and Safari data.")
                    .font(.title3)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
                Text("Rescan earlier scans with ⌘R to add the folders they skipped.")
                    .font(.callout)
                    .foregroundStyle(.tertiary)
            }
            Button("Start Scanning") { dismiss() }
                .buttonStyle(.glassProminent)
                .controlSize(.extraLarge)
                .keyboardShortcut(.defaultAction)
        }
    }

    /// Opens System Settings and moves this guide beside it, above other windows,
    /// so the steps stay in view.
    private func openSettings() {
        FullDiskAccess.openSettings()
        withAnimation(.smooth) { guide.openedSettings = true }
        guard let window, let screen = window.screen ?? NSScreen.main else { return }
        window.level = .floating
        let visible = screen.visibleFrame
        var frame = window.frame
        frame.origin.x = visible.maxX - frame.width - 24
        frame.origin.y = visible.midY - frame.height / 2
        window.setFrame(frame, display: true, animate: true)
    }
}

private struct Benefit: View {
    let icon: Icon
    let title: String
    let detail: String

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            IconImage(icon, weight: .duotone, size: 26)
                .foregroundStyle(.tint)
                .frame(width: 30)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.headline)
                Text(detail)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

private struct Step<Accessory: View>: View {
    let number: Int
    var done = false
    let title: String
    let detail: String
    @ViewBuilder let accessory: Accessory

    var body: some View {
        HStack(spacing: 14) {
            ZStack {
                Circle().fill(done ? Color.green.gradient : Color.accentColor.gradient)
                if done {
                    IconImage(.check, weight: .bold, size: 14)
                        .transition(.scale.combined(with: .opacity))
                } else {
                    Text("\(number)")
                }
            }
            .font(.callout.weight(.bold))
            .foregroundStyle(.white)
            .frame(width: 26, height: 26)
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.headline)
                Text(detail)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 8)
            accessory
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
    }
}

/// Vantage's own icon, draggable into the Full Disk Access list.
private struct AppDragTile: View {
    private var icon: Image { Image(nsImage: NSWorkspace.shared.icon(forFile: Bundle.main.bundlePath)) }

    var body: some View {
        VStack(spacing: 2) {
            icon
                .resizable()
                .frame(width: 40, height: 40)
            Text("Vantage")
                .font(.caption2.weight(.medium))
                .foregroundStyle(.secondary)
        }
        .frame(width: 64, height: 60)
        .glassEffect(.regular.interactive(), in: .rect(cornerRadius: 12))
        .pointerStyle(.grabIdle)
        .onDrag {
            NSItemProvider(object: Bundle.main.bundleURL as NSURL)
        } preview: {
            icon.resizable().frame(width: 64, height: 64)
        }
        .help("Drag into the Full Disk Access list in System Settings")
        .accessibilityLabel("Vantage app icon")
        .accessibilityHint("Drag into the Full Disk Access list in System Settings")
    }
}

/// Reports the hosting window, which SwiftUI doesn't expose for level and placement.
private struct WindowReader: NSViewRepresentable {
    let found: (NSWindow) -> Void

    func makeNSView(context: Context) -> NSView { Reader(found: found) }
    func updateNSView(_ nsView: NSView, context: Context) {}

    private final class Reader: NSView {
        let found: (NSWindow) -> Void

        init(found: @escaping (NSWindow) -> Void) {
            self.found = found
            super.init(frame: .zero)
        }

        required init?(coder: NSCoder) { fatalError("init(coder:) is not supported") }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let window { found(window) }
        }
    }
}
