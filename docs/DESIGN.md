# Vantage for Mac design

Vantage uses native macOS window controls and a translucent frame around opaque
content cards. An icon rail provides navigation, the toolbar holds location and
actions, and an optional inspector shows details about the selection. Liquid Glass
is reserved for controls that float above content.

## Design principles

- Native window controls, menus, popovers, Quick Look, and keyboard behavior remain familiar.
- Navigation, actions, data, and item details each have one home in the window.
- Selection actions appear when they apply. Tooltips and popovers keep the window uncluttered.
- Sizes, shares, and counts use aligned columns, tabular digits, and consistent category colors.
- Content stays readable in light and dark appearances, including at the minimum window size of 820 × 520.

## Window layout

`ContentView` wraps the welcome, scanning, failed, and browsing phases in
`AppShell`, so the surrounding controls remain in place as the phase changes.
The toolbar has no visible title or background. The window title remains available
to the Window menu and Mission Control.

| Region | Purpose | Component |
| --- | --- | --- |
| Navigation | Back, forward, and sidebar toggle | `NavigationButtons`, `RailToggle` |
| Location bar | Current folder or view, summary, and actions | `LocationBar`, `BrowsingBar` |
| Search | Replaces the path with a search field | `SearchButton`, `SearchField` |
| Rail | Views and categories; scan summary, New Scan, and Settings at the bottom | `Rail` |
| Content card | Current view's data | `AppShell`, `.contentCard()` |
| Inspector card | Selection details | `InspectorView` |
| Frame | Behind-window vibrancy and a faint accent wash | `WindowFrame` |

New views belong in `SidebarItem`, the rail, and `BrowserView.detail`. Selection
actions belong in `BrowsingBar`, `ItemMenu`, and the Item menu. Scan-wide warnings
belong in `SummaryCard`, with a badge dot on the rail's summary ring. Transient
confirmations use `AppModel.showToast(_:)`. App settings belong in `SettingsView`.
Menu commands live in `VantageCommands`, with shortcuts for common actions.

## Materials and dimensions

The frame uses `NSVisualEffectView` with `.underWindowBackground` and
behind-window blending. The toolbar and empty rail can drag the window.

Cards use `.contentCard()`: an opaque `controlBackgroundColor` fill, continuous
rounded corners, a hairline border, and a soft shadow. Glass is limited to floating
controls, selected rail tiles, popovers, toasts, hover cards, list footers, and
primary start-page buttons. Rows, tables, and card backgrounds have no glass fill.

Neighboring glass controls share a `GlassEffectContainer`. Glass attaches to the
content view so it cannot cover sibling glyphs. Rail selection uses
`.glassEffectID(_:in:)`; light tints and `.primary` glyphs preserve contrast.

Layout constants are defined in [Shell.swift](../mac/Sources/Vantage/Shell.swift)
and [CardList.swift](../mac/Sources/Vantage/CardList.swift). All values below are points.

| Constant | Value |
| --- | --- |
| `Chrome.railWidth` | 58 |
| `Chrome.railExpandedWidth` | 228 |
| `Chrome.railMinWidth`, `Chrome.railMaxWidth` | 190, 360 |
| `Chrome.railCollapseThreshold` | 130 |
| `Chrome.inset`, `Chrome.cardRadius` | 8, 16 |
| `Chrome.inspectorWidth` | 280 |
| `Chrome.locationBarWidth(window:)` | 46% of window width, limited to 340–720 |
| `CardList.inset`, `CardList.rowHeight`, `CardList.iconSize` | 16, 30, 22 |
| `CardList.sizeWidth`, `CardList.itemsWidth`, `ShareCell.width` | 76, 70, 170 |
| `CardList.compactWidth` | 640 |

Nested corner radii decrease by the inset. Treemap tiles begin at
`Chrome.cardRadius - 8` and step inward by 2 points per level.

## Interaction patterns

The collapsed rail shows icons and immediate `RailHoverLabel` labels. The expanded
rail adds titles, sizes or counts, and section headings. `RailRow` keeps icons in
the same position in both widths. The resize handle changes the saved width,
snaps closed below `Chrome.railCollapseThreshold`, and toggles on double-click.
Rail hover labels replace system tooltips.

The location bar shows a folder prompt on welcome, progress while scanning, a
warning on failure, and breadcrumbs or a view title while browsing. `BarButton`
actions form contextual selection and persistent view groups, divided by
`BarSeparator`. Breadcrumbs scroll when space is short. Search replaces the path
without resizing the toolbar.

Item lists use `List` with `.cardList()` and `.cardListRow()`. A pinned
`CardListHeader` provides sort controls. Rows have a middle-truncated name, an
optional subtitle, and trailing numeric columns with size last. `ShareCell` and
`SizeBar` show proportions. Below `CardList.compactWidth`, secondary columns
disappear while name and size remain. Item menus and Space for Quick Look remain
available. Empty and no-results states use `ContentUnavailableView`.

`TreemapSplit` provides a resizable split inside a card, using `SplitHandle` and a
saved fraction. Floating toasts, footers, and hover cards use capsule or rounded
rectangle glass, callout or caption text, and 12-point horizontal padding.

## Type, color, and icons

Text uses system styles: body for names, caption for secondary details, callout
for toolbar controls, and title styles on the start page and inspector header.
Semantic `.primary`, `.secondary`, and `.tertiary` colors support both appearances.
`Format` formats sizes, counts, and percentages; `monospacedDigit()` keeps them stable.

`FileCategory.color` supplies colors for category bars, treemap tiles, rail tints,
and icons. The accent color marks app actions and active toggles. Orange marks
warnings such as partial scans and Full Disk Access. Red marks destructive actions.

The app's own controls use Phosphor icons through `IconImage` or
`Label(_:icon:weight:size:)`. Context menus and the menu bar use SF Symbols.
[Icons.swift](../mac/Sources/Vantage/Icons.swift) defines named icons and system
fallbacks; `FileCategory.icon` supplies category icons.

| Icon weight | Use |
| --- | --- |
| Regular | Idle controls |
| Duotone | Selected or active controls, start-page tiles, and empty states |
| Bold | Small glyphs, carets, and icons marked `boxedDuotone` |
| Fill | Status marks |

`boxedDuotone` replaces the square duotone background of bare glyphs with bold.
Icon sizes are explicit: 20 points in the rail, 17–18 in the toolbar, 15–16 beside
text, 9–11 for carets, and about 48 in empty states. Template images use
`foregroundStyle`; the duotone layer retains 20% opacity.

[Icons.txt](../mac/Support/Icons.txt) lists the bundled assets. The app includes
Phosphor's MIT license as `Phosphor-LICENSE.txt`. The standalone Folio artwork is
[Folio.svg](../mac/Support/Artwork/Folio.svg); the layered app icon is
`mac/Support/AppIcon.icon`.

## Motion and accessibility

Selection and state changes use `.snappy`, panels use `.smooth`, and hover
transitions use short `.easeOut` animations. Changing counts use
`contentTransition(.numericText())`. Motion stays restrained so large data views
remain easy to follow.

Icon-only controls have an accessibility label and an action tooltip with any
shortcut. Rail items provide their title and detail through the hover label and
accessibility value. Selected controls expose `.isSelected`; custom tap targets
expose `.isButton` and an `accessibilityAction`.

Common shortcuts are ⌘O to scan a folder, ⌘R to rescan, ⌥⌘R for a full rescan,
⌘F to search, Esc to close search, ⌃⌘S for the sidebar, ⌘1 and ⌘2 for views,
⌥⌘I for the inspector, Space for Quick Look, ⌘[ and ⌘] for history,
⌘↑ for the enclosing folder, and ⌘⌫ to move items to Trash.
