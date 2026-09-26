import AppKit
import SwiftUI

/// Window layout: the native traffic lights and toolbar over a translucent
/// frame, an icon rail down the leading edge, and the content in inset cards.
enum Chrome {
    static let railWidth: CGFloat = 58
    static let railPadding: CGFloat = 10
    /// The expanded rail's default and drag limits. Dragging it narrower than
    /// `railCollapseThreshold` snaps it to the collapsed rail.
    static let railExpandedWidth: CGFloat = 228
    static let railMinWidth: CGFloat = 190
    static let railMaxWidth: CGFloat = 360
    static let railCollapseThreshold: CGFloat = 130
    static let inset: CGFloat = 8
    static let cardRadius: CGFloat = 16
    static let inspectorWidth: CGFloat = 280

    /// The location bar grows with the window but stays readable.
    static func locationBarWidth(window: CGFloat) -> CGFloat {
        min(max(window * 0.46, 340), 720)
    }
}

struct AppShell<Content: View>: View {
    @Environment(AppModel.self) private var model
    @ViewBuilder var content: Content

    var body: some View {
        HStack(spacing: 0) {
            Rail()
                .zIndex(1)   // hover labels extend over the card
            HStack(spacing: Chrome.inset) {
                content
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentCard()
                if model.inspectorShown, model.isBrowsing, let browser = model.browser {
                    // Node indices belong to one tree; a replacement tree rebuilds these views.
                    InspectorView()
                        .environment(browser)
                        .id(browser.tree.id)
                        .frame(width: Chrome.inspectorWidth)
                        .frame(maxHeight: .infinity)
                        .contentCard()
                        .transition(.move(edge: .trailing).combined(with: .opacity))
                }
            }
            .padding([.trailing, .bottom], Chrome.inset)
        }
        .background { WindowFrame().ignoresSafeArea() }
        .animation(.smooth(duration: 0.32), value: model.inspectorShown)
    }
}

/// Behind-window vibrancy with a faint accent wash, shown around the cards.
private struct WindowFrame: View {
    var body: some View {
        ZStack {
            VisualEffectBackground(material: .underWindowBackground)
            LinearGradient(colors: [.accentColor.opacity(0.07), .clear, FileCategory.image.color.opacity(0.03)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
        }
    }
}

private struct VisualEffectBackground: NSViewRepresentable {
    let material: NSVisualEffectView.Material

    func makeNSView(context: Context) -> NSVisualEffectView {
        let view = NSVisualEffectView()
        view.material = material
        view.blendingMode = .behindWindow
        view.state = .followsWindowActiveState
        return view
    }

    func updateNSView(_ view: NSVisualEffectView, context: Context) {
        view.material = material
    }
}

private struct ContentCard: ViewModifier {
    @Environment(\.colorScheme) private var colorScheme

    func body(content: Content) -> some View {
        let shape = RoundedRectangle(cornerRadius: Chrome.cardRadius, style: .continuous)
        content
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(shape)
            .overlay {
                shape.strokeBorder(Color.primary.opacity(colorScheme == .dark ? 0.12 : 0.08), lineWidth: 0.5)
            }
            .shadow(color: .black.opacity(colorScheme == .dark ? 0.35 : 0.08), radius: 12, y: 4)
            .shadow(color: .black.opacity(0.05), radius: 1, y: 0.5)
    }
}

extension View {
    func contentCard() -> some View { modifier(ContentCard()) }
}

// MARK: - Rail

struct Rail: View {
    @Environment(AppModel.self) private var model
    @Namespace private var selection

    var body: some View {
        let expanded = model.railExpanded
        GlassEffectContainer(spacing: 4) {
            VStack(alignment: .leading, spacing: 4) {
                if model.isBrowsing, let browser = model.browser {
                    let _ = browser.revision
                    let tree = browser.tree
                    RailButton(title: "Folders", icon: .folders, detail: Format.bytes(tree.totalSize),
                               selected: current == .folders, namespace: selection) { select(.folders) }
                    RailButton(title: "Largest Files", icon: .largestFiles, detail: "\(Format.count(tree.fileCount)) files",
                               selected: current == .largestFiles, namespace: selection) { select(.largestFiles) }
                    RailSection(title: "Kinds")
                    ScrollView(.vertical) {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(kinds(tree), id: \.self) { category in
                                RailButton(title: category.title, icon: category.icon, tint: category.color,
                                           detail: Format.bytes(tree.categoryTotals[category] ?? 0),
                                           selected: current == .category(category), namespace: selection) {
                                    select(.category(category))
                                }
                            }
                        }
                    }
                    .scrollIndicators(.never)
                    .scrollClipDisabled()
                    .transition(.opacity)
                }
                Spacer(minLength: 8)
                if model.isBrowsing, let browser = model.browser {
                    SummaryButton()
                        .environment(browser)
                        .id(browser.tree.id)
                }
                RailButton(title: "New Scan", icon: .add, detail: isWelcome ? "Choose a folder or disk" : "Back to the start page",
                           selected: isWelcome, namespace: selection) {
                    if isWelcome { model.chooseFolder() } else { model.closeScan() }
                }
                RailSettingsButton()
            }
            .padding(.horizontal, Chrome.railPadding)
            .padding(.top, 4)
            .padding(.bottom, Chrome.inset + 2)
        }
        .frame(width: model.railDisplayWidth, alignment: .leading)
        .frame(maxHeight: .infinity)
        .overlay(alignment: .trailing) {
            // Straddles the gap and the card's leading edge.
            RailResizeHandle()
                .offset(x: RailResizeHandle.width / 2)
        }
        .background {
            // The empty rail moves the window, like the title bar above it.
            Color.clear
                .contentShape(.rect)
                .gesture(WindowDragGesture())
                .allowsWindowActivationEvents(true)
        }
        .environment(\.railExpanded, expanded)
        .animation(.snappy(duration: 0.28), value: current)
        .animation(.smooth, value: model.isBrowsing)
    }

    private var isWelcome: Bool { if case .welcome = model.phase { true } else { false } }

    private var current: SidebarItem? {
        model.isBrowsing ? (model.sidebar) : nil
    }

    private func select(_ item: SidebarItem) {
        model.sidebar = item
    }

    private func kinds(_ tree: ScanTree) -> [FileCategory] {
        FileCategory.fileKinds
            .filter { (tree.categoryTotals[$0] ?? 0) > 0 }
            .sorted { (tree.categoryTotals[$0] ?? 0) > (tree.categoryTotals[$1] ?? 0) }
    }
}

/// Drags the rail's width. The expanded width resists past its limits and
/// settles on release; below the collapse threshold the rail snaps to icons,
/// and dragging a collapsed rail out past it snaps the sidebar open.
private struct RailResizeHandle: View {
    @Environment(AppModel.self) private var model
    @State private var startWidth: CGFloat?
    @State private var hovering = false

    static let width: CGFloat = 10

    var body: some View {
        Color.clear
            .frame(width: Self.width)
            .frame(maxHeight: .infinity)
            .contentShape(.rect)
            .overlay {
                Capsule()
                    .fill(Color.primary.opacity(startWidth != nil ? 0.32 : 0.18))
                    .frame(width: 3, height: 40)
                    .opacity(hovering || startWidth != nil ? 1 : 0)
                    .animation(.easeOut(duration: 0.15), value: hovering)
            }
            .pointerStyle(.columnResize)
            .onHover { hovering = $0 }
            .gesture(
                // Global space: the handle moves with the drag, so local translations would feed back.
                DragGesture(minimumDistance: 1, coordinateSpace: .global)
                    .onChanged { value in
                        let start = startWidth ?? model.railDisplayWidth
                        startWidth = start
                        model.dragRail(to: start + value.translation.width)
                    }
                    .onEnded { _ in
                        startWidth = nil
                        model.endRailDrag()
                    }
            )
            .onTapGesture(count: 2) { model.toggleRail() }
            .help("Drag to resize the sidebar; double-click to \(model.railExpanded ? "collapse" : "expand") it")
            .accessibilityHidden(true)
    }
}

extension EnvironmentValues {
    /// The rail shows labels beside its icons instead of hover labels.
    @Entry var railExpanded = false
}

/// A divider when collapsed; a small caption when expanded.
private struct RailSection: View {
    let title: String
    @Environment(\.railExpanded) private var expanded

    var body: some View {
        ZStack(alignment: .leading) {
            Capsule()
                .fill(Color.primary.opacity(0.12))
                .frame(width: 18, height: 1)
                .padding(.leading, (RailRow<EmptyView>.collapsedSize - 18) / 2)
                .opacity(expanded ? 0 : 1)
            Text(title)
                .font(.caption.weight(.semibold))
                .textCase(.uppercase)
                .kerning(0.4)
                .foregroundStyle(.tertiary)
                .padding(.leading, 8)
                .opacity(expanded ? 1 : 0)
        }
        .frame(height: 18)
        .accessibilityHidden(true)
    }
}

/// One rail destination. The selection is a tinted glass tile that slides between items.
private struct RailButton: View {
    let title: String
    let icon: Icon
    var tint: Color = .accentColor
    var detail: String?
    let selected: Bool
    let namespace: Namespace.ID
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            RailRow(title: title, detail: detail, tint: tint, selected: selected, hovering: hovering, namespace: namespace) {
                IconImage(icon, weight: selected ? .duotone : .regular, size: 20)
                    .foregroundStyle(selected ? AnyShapeStyle(.primary) : hovering ? AnyShapeStyle(tint) : AnyShapeStyle(.secondary))
            }
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .accessibilityLabel(title)
        .accessibilityValue(detail ?? "")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }
}

/// The shared shape of rail items. The icon keeps its place in both widths, so
/// expanding only reveals the title and detail beside it. Collapsed, hovering
/// shows the same text in a label beside the rail straight away.
private struct RailRow<Icon: View>: View {
    let title: String
    var detail: String?
    var tint: Color = .accentColor
    var selected = false
    var hovering = false
    let namespace: Namespace.ID
    @ViewBuilder var icon: Icon
    @Environment(\.railExpanded) private var expanded

    static var collapsedSize: CGFloat { 38 }
    private static var iconSize: CGFloat { 22 }
    private var shape: RoundedRectangle { RoundedRectangle(cornerRadius: 12, style: .continuous) }

    var body: some View {
        HStack(spacing: 10) {
            icon
                .frame(width: Self.iconSize, height: Self.iconSize)
            if expanded {
                VStack(alignment: .leading, spacing: 0) {
                    Text(title)
                        .font(.callout.weight(.medium))
                        .foregroundStyle(selected || hovering ? .primary : .secondary)
                    if let detail {
                        Text(detail)
                            .font(.caption)
                            .monospacedDigit()
                            .foregroundStyle(.tertiary)
                    }
                }
                .lineLimit(1)
                .transition(.opacity)
                Spacer(minLength: 0)
            }
        }
        .padding(.horizontal, (Self.collapsedSize - Self.iconSize) / 2)
        .frame(width: expanded ? nil : Self.collapsedSize, height: Self.collapsedSize, alignment: .leading)
        .frame(maxWidth: expanded ? .infinity : nil, alignment: .leading)
        .background {
            if hovering && !selected {
                shape.fill(Color.primary.opacity(0.07))
            }
        }
        // Content must sit inside the glass, or the container draws the glass over it.
        .glassEffect(selected ? .regular.tint(tint.opacity(0.32)).interactive() : .identity, in: shape)
        .glassEffectID(selected ? "rail.selection" : nil, in: namespace)
        .contentShape(shape)
        .overlay(alignment: .leading) {
            if hovering && !expanded {
                RailHoverLabel(title: title, detail: detail)
                    .offset(x: Self.collapsedSize + 10)
                    .transition(.opacity.combined(with: .offset(x: -4)))
            }
        }
        .animation(.easeOut(duration: 0.12), value: hovering)
    }
}

/// The name of a collapsed rail item, shown beside it on hover.
private struct RailHoverLabel: View {
    let title: String
    let detail: String?

    var body: some View {
        HStack(spacing: 6) {
            Text(title)
                .fontWeight(.medium)
            if let detail {
                Text(detail)
                    .foregroundStyle(.secondary)
                    .monospacedDigit()
            }
        }
        .font(.callout)
        .lineLimit(1)
        .fixedSize()
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        // Denser than plain glass: the label often sits over busy lists and tiles.
        .glassEffect(.regular.tint(Color(nsColor: .windowBackgroundColor).opacity(0.8)), in: .capsule)
        .shadow(color: .black.opacity(0.18), radius: 8, y: 2)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

private struct RailSettingsButton: View {
    @State private var hovering = false
    @Namespace private var namespace

    var body: some View {
        SettingsLink {
            RailRow(title: "Settings", detail: "⌘,", hovering: hovering, namespace: namespace) {
                IconImage(.settings, size: 20)
                    .foregroundStyle(hovering ? .primary : .secondary)
            }
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .accessibilityLabel("Settings")
    }
}

/// A ring of the scan's kinds; opens the scan summary.
private struct SummaryButton: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    @State private var hovering = false
    @State private var shown = false
    @Namespace private var namespace

    var body: some View {
        let _ = browser.revision
        let tree = browser.tree
        let detail = "\(Format.bytes(tree.totalSize)) scanned\(tree.isPartial ? " · incomplete" : "")"
        Button { shown.toggle() } label: {
            RailRow(title: "Scan Summary", detail: detail, hovering: hovering || shown, namespace: namespace) {
                UsageRing(tree: tree)
                    .frame(width: 20, height: 20)
                    .overlay(alignment: .topTrailing) {
                        if tree.isPartial {
                            Circle()
                                .fill(.orange)
                                .frame(width: 7, height: 7)
                                .overlay { Circle().stroke(Color(nsColor: .windowBackgroundColor), lineWidth: 1.5) }
                                .offset(x: 4, y: -4)
                        }
                    }
            }
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .accessibilityLabel("Scan Summary")
        .accessibilityValue(detail)
        .popover(isPresented: $shown, arrowEdge: .trailing) {
            SummaryCard()
                .environment(model)
                .environment(browser)
                .frame(width: 270)
        }
    }
}

struct UsageRing: View {
    let tree: ScanTree
    var lineWidth: CGFloat = 3.5

    private struct Segment: Identifiable {
        let id: FileCategory
        let start: Double
        let end: Double
    }

    var body: some View {
        ZStack {
            Circle().stroke(Color.primary.opacity(0.12), lineWidth: lineWidth)
            ForEach(segments) { segment in
                Circle()
                    .trim(from: segment.start, to: segment.end)
                    .stroke(segment.id.color.gradient, style: StrokeStyle(lineWidth: lineWidth, lineCap: .butt))
            }
        }
        .rotationEffect(.degrees(-90))
        .padding(lineWidth / 2)
    }

    private var segments: [Segment] {
        let total = Double(max(tree.totalSize, 1))
        let parts = FileCategory.fileKinds
            .map { ($0, Double(tree.categoryTotals[$0] ?? 0) / total) }
            .filter { $0.1 > 0.004 }
            .sorted { $0.1 > $1.1 }
        var start = 0.0
        return parts.map { category, share in
            defer { start += share }
            let gap = parts.count > 1 && share > 0.03 ? 0.012 : 0
            return Segment(id: category, start: start, end: min(start + share - gap, 1))
        }
    }
}

// MARK: - Toolbar

/// Back and forward, beside the window controls.
struct NavigationButtons: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        ControlGroup {
            Button { model.goBack() } label: { Label("Back", icon: .back, weight: .bold, size: 15) }
                .disabled(!model.canGoBack)
            Button { model.goForward() } label: { Label("Forward", icon: .forward, weight: .bold, size: 15) }
                .disabled(!model.canGoForward)
        }
        .controlGroupStyle(.navigation)
        .help("See folders you viewed previously")
    }
}

/// Expands the rail into a labelled sidebar, or collapses it back to icons.
struct RailToggle: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button { model.toggleRail() } label: {
            Label(model.railExpanded ? "Hide Sidebar" : "Show Sidebar", icon: .sidebar,
                  weight: model.railExpanded ? .duotone : .regular, size: 18)
        }
        .help(model.railExpanded ? "Collapse the sidebar (⌃⌘S)" : "Expand the sidebar (⌃⌘S)")
    }
}

/// The wide toolbar capsule: where you are, what it weighs, and what you can do with it.
struct LocationBar: View {
    @Environment(AppModel.self) private var model
    let width: CGFloat

    var body: some View {
        HStack(spacing: 6) {
            switch model.phase {
            case .welcome:
                // A toolbar drops a Button whose label holds a Spacer, so the whole bar takes the click.
                HStack(spacing: 8) {
                    Image(nsImage: NSApp.applicationIconImage)
                        .resizable()
                        .frame(width: 18, height: 18)
                    Text("Choose a folder or disk to scan")
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 8)
                    KeyHint("⌘O")
                }
                .contentShape(.rect)
                .onTapGesture { model.chooseFolder() }
                .accessibilityElement(children: .combine)
                .accessibilityAddTraits(.isButton)
                .accessibilityAction { model.chooseFolder() }
            case .scanning(let url):
                ProgressView()
                    .controlSize(.small)
                    .scaleEffect(0.8)
                Text("\(model.isOpeningSaved ? "Opening" : "Scanning") \(Format.displayName(url))")
                    .fontWeight(.medium)
                    .lineLimit(1)
                Spacer(minLength: 8)
                if model.scanProgress.entries > 0 {
                    Text("\(Format.count(Int(model.scanProgress.entries))) items")
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                        .contentTransition(.numericText())
                        .animation(.snappy, value: model.scanProgress.entries)
                }
            case .failed(let url, _):
                IconImage(.warning, weight: .fill, size: 16)
                    .foregroundStyle(.orange)
                Text("Couldn’t scan \(Format.displayName(url))")
                    .lineLimit(1)
                Spacer(minLength: 0)
            case .browsing:
                if let browser = model.browser {
                    BrowsingBar()
                        .environment(browser)
                        .id(browser.tree.id)
                }
            }
        }
        .font(.callout)
        .padding(.leading, 12)
        .padding(.trailing, 6)
        .frame(width: width, height: 30)
    }
}

private struct KeyHint: View {
    let keys: String
    init(_ keys: String) { self.keys = keys }

    var body: some View {
        Text(keys)
            .font(.caption.weight(.medium))
            .foregroundStyle(.tertiary)
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(Color.primary.opacity(0.06), in: .capsule)
    }
}

private struct BrowsingBar: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    @AppStorage(Preferences.showTreemap) private var showTreemap = true
    private var tree: ScanTree { browser.tree }

    var body: some View {
        let _ = browser.revision
        let selected = browser.selectedItems
        Group {
            if model.isSearching {
                SearchField()
                    .transition(.opacity.combined(with: .scale(scale: 0.98, anchor: .leading)))
            } else {
                // The path keeps priority: the summary shortens, then hides, when actions crowd the bar.
                ViewThatFits(in: .horizontal) {
                    locationAndSummary(summary)
                    locationAndSummary(shortSummary)
                    location
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .transition(.opacity)
            }
        }

        BarSeparator()

        if !selected.isEmpty {
            Group {
                BarButton("Quick Look", icon: .quickLook, help: "Preview the selected item (Space)") { model.quickLook() }
                BarButton("Show in Finder", icon: .reveal, help: "Show the selected items in Finder") { model.reveal(selected) }
                BarButton("Move to Trash", icon: .trash, help: "Move the selected items to the Trash (⌘⌫)") {
                    model.requestDeletion(selected)
                }
                BarSeparator()
            }
            .transition(.opacity.combined(with: .scale(scale: 0.8)))
        }
        if model.sidebar == .folders {
            BarButton("Treemap", icon: .treemap, active: showTreemap,
                      help: showTreemap ? "Hide the treemap" : "Show the treemap") { showTreemap.toggle() }
        }
        if model.isUpdating {
            ProgressView()
                .controlSize(.small)
                .scaleEffect(0.7)
                .frame(width: 26, height: 26)
                .help("Checking for changes since the last scan…")
        } else {
            BarButton("Rescan", icon: .rescan,
                      help: "Update with changes since the last scan (⌘R). Full Rescan: ⌥⌘R") { model.rescan() }
        }
        BarButton("Info", icon: .sidebar, mirrored: true, active: model.inspectorShown,
                  help: "Show details about the selection (⌥⌘I)") { model.inspectorShown.toggle() }
            .keyboardShortcut("i", modifiers: [.command, .option])
    }

    @ViewBuilder
    private var location: some View {
        switch model.sidebar {
        case .folders:
            Breadcrumbs()
        case .largestFiles:
            BarTitle(icon: .largestFiles, tint: .accentColor, title: "Largest Files")
        case .category(let category):
            BarTitle(icon: category.icon, tint: category.color, title: category.title)
        }
    }

    private func locationAndSummary(_ text: String) -> some View {
        HStack(spacing: 8) {
            location
            Spacer(minLength: 8)
            Text(text)
                .foregroundStyle(.secondary)
                .monospacedDigit()
                .lineLimit(1)
                .fixedSize()
        }
    }

    private var shortSummary: String {
        if model.isUpdating { return "Updating…" }
        switch model.sidebar {
        case .folders: return Format.bytes(tree.size[browser.directory])
        case .largestFiles: return Format.count(tree.fileCount)
        case .category(let category): return Format.bytes(tree.categoryTotals[category] ?? 0)
        }
    }

    private var summary: String {
        if model.isUpdating { return "Updating…" }
        switch model.sidebar {
        case .folders:
            let d = browser.directory
            return "\(Format.bytes(tree.size[d])) · \(Format.items(Int(tree.items[d])))"
        case .largestFiles:
            return "\(Format.count(tree.fileCount)) files"
        case .category(let category):
            return Format.bytes(tree.categoryTotals[category] ?? 0)
        }
    }
}

private struct BarTitle: View {
    let icon: Icon
    let tint: Color
    let title: String

    var body: some View {
        HStack(spacing: 7) {
            IconImage(icon, weight: .duotone, size: 17)
                .foregroundStyle(tint)
            Text(title)
                .fontWeight(.medium)
                .lineLimit(1)
        }
    }
}

/// The folder path, scrolled to its end; each part navigates there.
private struct Breadcrumbs: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    private var tree: ScanTree { browser.tree }

    var body: some View {
        let chain = tree.lineage(browser.directory)
        ScrollView(.horizontal) {
            HStack(spacing: 1) {
                ForEach(Array(chain.enumerated()), id: \.element) { index, item in
                    if index > 0 {
                        IconImage(.forward, weight: .bold, size: 10)
                            .foregroundStyle(.tertiary)
                    }
                    Crumb(item: item, current: item == browser.directory)
                }
            }
            .padding(.vertical, 2)
        }
        .scrollIndicators(.never)
        // Short paths sit at the leading edge; long ones open scrolled to the current folder.
        .defaultScrollAnchor(.leading, for: .alignment)
        .defaultScrollAnchor(.trailing, for: .initialOffset)
        .defaultScrollAnchor(.trailing, for: .sizeChanges)
        .mask {
            // Fade parts that scroll under the leading edge.
            HStack(spacing: 0) {
                LinearGradient(colors: [.clear, .black], startPoint: .leading, endPoint: .trailing)
                    .frame(width: chain.count > 2 ? 10 : 0)
                Color.black
            }
        }
    }
}

private struct Crumb: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    let item: Int
    let current: Bool
    @State private var hovering = false

    var body: some View {
        let tree = browser.tree
        Button { model.navigate(to: item) } label: {
            HStack(spacing: 5) {
                ItemIcon(tree: tree, item: item, size: 15)
                Text(tree.name(item))
                    .fontWeight(current ? .semibold : .regular)
                    .foregroundStyle(current ? .primary : .secondary)
                    .lineLimit(1)
            }
            .padding(.horizontal, 5)
            .padding(.vertical, 3)
            .background(Color.primary.opacity(hovering && !current ? 0.07 : 0), in: .capsule)
            .contentShape(.capsule)
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .help(tree.url(item).path)
    }
}

private struct SearchField: View {
    @Environment(AppModel.self) private var model
    @Environment(BrowserState.self) private var browser
    @FocusState private var focused: Bool

    var body: some View {
        @Bindable var model = model
        HStack(spacing: 6) {
            IconImage(.search, weight: .bold, size: 15)
                .foregroundStyle(.secondary)
            TextField(prompt, text: $model.searchText)
                .textFieldStyle(.plain)
                .focused($focused)
                .onExitCommand { model.endSearch() }
                .onKeyPress(.escape) {
                    model.endSearch()
                    return .handled
                }
            if !model.searchText.isEmpty {
                Button { model.searchText = "" } label: { Label("Clear", icon: .clear, weight: .fill, size: 15) }
                    .labelStyle(.iconOnly)
                    .buttonStyle(.plain)
                    .foregroundStyle(.tertiary)
            }
        }
        .frame(maxWidth: .infinity)
        .onAppear { focused = true }
    }

    private var prompt: String {
        model.sidebar == .folders
            ? "Search in \(browser.tree.name(browser.directory))"
            : "Search \(model.sidebar.title)"
    }
}

private struct BarButton: View {
    let title: String
    let icon: Icon
    var mirrored = false
    var active = false
    let help: String
    let action: () -> Void
    @State private var hovering = false

    init(_ title: String, icon: Icon, mirrored: Bool = false, active: Bool = false, help: String, action: @escaping () -> Void) {
        self.title = title
        self.icon = icon
        self.mirrored = mirrored
        self.active = active
        self.help = help
        self.action = action
    }

    var body: some View {
        Button(action: action) {
            IconImage(icon, weight: active ? .duotone : .regular, size: 17)
                .scaleEffect(x: mirrored ? -1 : 1)
                .foregroundStyle(active ? AnyShapeStyle(.tint) : AnyShapeStyle(hovering ? .primary : .secondary))
                .frame(width: 26, height: 26)
                .background(Circle().fill(active ? Color.accentColor.opacity(0.14) : Color.primary.opacity(hovering ? 0.08 : 0)))
                .contentShape(.circle)
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .help(help)
        .accessibilityLabel(title)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

private struct BarSeparator: View {
    var body: some View {
        Rectangle()
            .fill(Color.primary.opacity(0.12))
            .frame(width: 1, height: 16)
            .padding(.horizontal, 3)
    }
}

/// The search button at the trailing end; it turns the location bar into a search field.
struct SearchButton: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        Button {
            model.isSearching ? model.endSearch() : model.beginSearch()
        } label: {
            Label(model.isSearching ? "Close Search" : "Search", icon: model.isSearching ? .close : .search,
                  weight: model.isSearching ? .regular : .bold, size: 17)
        }
        .disabled(!model.isBrowsing)
        .help(model.isSearching ? "Close search (Esc)" : "Search (⌘F)")
    }
}

extension SidebarItem {
    var title: String {
        switch self {
        case .folders: "Folders"
        case .largestFiles: "Largest Files"
        case .category(let category): category.title
        }
    }
}

extension AppModel {
    func toggleRail() {
        withAnimation(.snappy(duration: 0.3)) { railExpanded.toggle() }
    }

    var railDisplayWidth: CGFloat { railExpanded ? railExpandedWidth : Chrome.railWidth }

    /// Follows a drag of the rail's edge to `proposed`, snapping between collapsed and expanded.
    func dragRail(to proposed: CGFloat) {
        let collapse = proposed < Chrome.railCollapseThreshold
        if collapse == railExpanded {
            NSHapticFeedbackManager.defaultPerformer.perform(.alignment, performanceTime: .now)
            if !collapse { railExpandedWidth = Self.resisted(proposed) }
            withAnimation(.snappy(duration: 0.26)) { railExpanded = !collapse }
            return
        }
        if !collapse { railExpandedWidth = Self.resisted(proposed) }
    }

    func endRailDrag() {
        let settled = min(max(railExpandedWidth, Chrome.railMinWidth), Chrome.railMaxWidth)
        if settled != railExpandedWidth {
            withAnimation(.snappy(duration: 0.26)) { railExpandedWidth = settled }
        }
        UserDefaults.standard.set(Double(settled), forKey: Preferences.railWidth)
    }

    /// Rubber-bands widths past the limits so the edge pushes back before it settles.
    private static func resisted(_ width: CGFloat) -> CGFloat {
        if width < Chrome.railMinWidth { return Chrome.railMinWidth - (Chrome.railMinWidth - width) * 0.3 }
        if width > Chrome.railMaxWidth { return Chrome.railMaxWidth + (width - Chrome.railMaxWidth) * 0.2 }
        return width
    }

    func beginSearch() {
        guard isBrowsing else { return }
        withAnimation(.snappy(duration: 0.25)) { isSearching = true }
    }

    func endSearch() {
        withAnimation(.snappy(duration: 0.25)) {
            searchText = ""
            isSearching = false
        }
    }
}

extension Format {
    static func displayName(_ url: URL) -> String {
        "“\(url.lastPathComponent.isEmpty ? url.path : url.lastPathComponent)”"
    }
}
