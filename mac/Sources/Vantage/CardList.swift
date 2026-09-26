import SwiftUI

/// Lists drawn straight on the content card: a pinned sort header over rounded,
/// separator-free rows that share the shell's type and spacing.
enum CardList {
    static let iconSize: CGFloat = 22
    static let rowHeight: CGFloat = 30
    /// Leading inset shared by the header and the row content.
    static let inset: CGFloat = 16
    static let sizeWidth: CGFloat = 76
    static let itemsWidth: CGFloat = 70
    static let percentWidth: CGFloat = 40
    /// Below this width the list drops its secondary columns.
    static let compactWidth: CGFloat = 640
}

extension View {
    /// The list chrome shared by the folder and file lists.
    func cardList() -> some View {
        listStyle(.inset)
            .alternatingRowBackgrounds(.disabled)
            .scrollContentBackground(.hidden)
            .environment(\.defaultMinListRowHeight, CardList.rowHeight)
    }

    func cardListRow() -> some View {
        listRowInsets(EdgeInsets(top: 0, leading: CardList.inset - 10, bottom: 0, trailing: CardList.inset - 10))
            .listRowSeparator(.hidden)
    }
}

// MARK: - Sorting

typealias RowSort = [KeyPathComparator<ItemRow>]

/// A header label that sorts the list by one key; clicking again reverses it.
struct SortButton<Value: Comparable>: View {
    let title: String
    let key: KeyPath<ItemRow, Value>
    var descendingFirst = false
    var alignment: Alignment = .leading
    var fills = true
    @Binding var order: RowSort
    @State private var hovering = false

    private var active: KeyPathComparator<ItemRow>? {
        order.first.flatMap { $0.keyPath == key ? $0 : nil }
    }

    var body: some View {
        Button {
            if let active {
                order = [KeyPathComparator(key, order: active.order == .forward ? .reverse : .forward)]
            } else {
                order = [KeyPathComparator(key, order: descendingFirst ? .reverse : .forward)]
            }
        } label: {
            HStack(spacing: 3) {
                Text(title)
                if let active {
                    IconImage(active.order == .forward ? .caretUp : .caretDown, weight: .bold, size: 9)
                }
            }
            .foregroundStyle(active != nil ? AnyShapeStyle(.primary) : AnyShapeStyle(hovering ? .primary : .secondary))
            .frame(maxWidth: fills ? .infinity : nil, alignment: alignment)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .help("Sort by \(title)")
        .accessibilityAddTraits(active != nil ? .isSelected : [])
    }
}

/// The pinned row of sort labels above a card list.
struct CardListHeader<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        HStack(spacing: 12) {
            content
        }
        .font(.caption.weight(.semibold))
        .textCase(.uppercase)
        .kerning(0.4)
        .padding(.leading, CardList.inset + CardList.iconSize + 16)
        .padding(.trailing, CardList.inset + 6)
        .frame(height: 30)
    }
}

// MARK: - Rows

private struct RowIcon: View {
    let tree: ScanTree
    let item: Int

    var body: some View {
        ItemIcon(tree: tree, item: item, size: CardList.iconSize)
    }
}

struct FolderRowView: View {
    let tree: ScanTree
    let row: ItemRow
    let parentSize: UInt64
    let searching: Bool
    let compact: Bool

    var body: some View {
        HStack(spacing: 12) {
            HStack(spacing: 10) {
                RowIcon(tree: tree, item: row.id)
                VStack(alignment: .leading, spacing: 1) {
                    Text(row.name)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if searching {
                        Text(row.location)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .truncationMode(.head)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            if !compact {
                ShareCell(fraction: parentSize > 0 ? Double(row.size) / Double(parentSize) : 0,
                          label: Format.percent(row.size, of: parentSize), color: row.category.color)
                Text(row.isDirectory ? Format.count(row.items) : "")
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .frame(width: CardList.itemsWidth, alignment: .trailing)
            }
            Text(Format.bytes(row.size))
                .fontWeight(.medium)
                .monospacedDigit()
                .frame(width: CardList.sizeWidth, alignment: .trailing)
        }
        .padding(.vertical, searching ? 3 : 0)
        .frame(minHeight: CardList.rowHeight)
    }
}

struct FileRowView: View {
    let tree: ScanTree
    let row: ItemRow
    let largest: UInt64
    let compact: Bool

    var body: some View {
        HStack(spacing: 12) {
            HStack(spacing: 10) {
                RowIcon(tree: tree, item: row.id)
                VStack(alignment: .leading, spacing: 1) {
                    Text(row.name)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    Text(row.location)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.head)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            if !compact {
                ShareCell(fraction: Double(row.size) / Double(max(largest, 1)),
                          label: Format.percent(row.size, of: tree.totalSize), color: row.category.color)
                    .help(Format.percent(row.size, of: tree.totalSize) + " of everything scanned")
            }
            Text(Format.bytes(row.size))
                .fontWeight(.medium)
                .monospacedDigit()
                .frame(width: CardList.sizeWidth, alignment: .trailing)
        }
        .padding(.vertical, 4)
        .frame(minHeight: CardList.rowHeight + 8)
    }
}

/// A proportional bar and its percentage, sized to the list's share column.
struct ShareCell: View {
    let fraction: Double
    let label: String
    let color: Color

    static let width: CGFloat = 170

    var body: some View {
        HStack(spacing: 8) {
            SizeBar(fraction: fraction, color: color)
            Text(label)
                .font(.caption)
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .frame(width: CardList.percentWidth, alignment: .trailing)
        }
        .frame(width: Self.width)
    }
}
