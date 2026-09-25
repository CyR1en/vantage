import Foundation

/// Shared SVX1 encoder for new inventories and saved trees. Parent indexes may
/// refer forward or backward; an index equal to the entry count is the root.
struct ScanExportWriter {
    private(set) var data = Data()

    init(info: ScanInfo, count: Int, namesSize: Int, totalSize: UInt64,
         fileCount: UInt64, folderCount: UInt64) {
        data.reserveCapacity(96 + info.rootURL.path.utf8.count + info.reason.utf8.count
                             + info.method.utf8.count + count * 29 + namesSize)
        data.append(contentsOf: "SVX1".utf8)
        u32(info.completion.rawValue)
        u32(info.allocated ? 1 : 0)
        u64(UInt64(info.scanDuration * 1e9))
        for value in [totalSize, totalSize, fileCount, folderCount, 0, info.permissionErrors, 0] {
            u64(value)
        }
        text(info.rootURL.path)
        text(info.reason)
        text(info.method)
        u64(UInt64(count))
    }

    mutating func append(parent: Int, kind: UInt8, size: UInt64, name: ArraySlice<UInt8>) {
        u64(UInt64(parent))
        data.append(kind)
        u64(kind == TreeBuilder.file ? size : 0)
        u64(0)
        u32(UInt32(name.count))
        data.append(contentsOf: name)
    }

    private mutating func u32(_ value: UInt32) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    private mutating func u64(_ value: UInt64) {
        withUnsafeBytes(of: value.littleEndian) { data.append(contentsOf: $0) }
    }

    private mutating func text(_ value: String) {
        u32(UInt32(value.utf8.count))
        data.append(contentsOf: value.utf8)
    }
}
