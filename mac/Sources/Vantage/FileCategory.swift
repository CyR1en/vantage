import SwiftUI

enum FileCategory: UInt8, CaseIterable, Identifiable, Hashable, Sendable {
    case folder, video, image, audio, archive, application, document, developer, other

    var id: Self { self }

    static var fileKinds: [FileCategory] { allCases.filter { $0 != .folder } }

    var title: String {
        switch self {
        case .folder: "Folders"
        case .video: "Movies"
        case .image: "Pictures"
        case .audio: "Music & Audio"
        case .archive: "Archives & Disk Images"
        case .application: "Apps & Libraries"
        case .document: "Documents"
        case .developer: "Developer"
        case .other: "Other"
        }
    }

    var icon: Icon {
        switch self {
        case .folder: Icon(name: "folder", fallback: "folder.fill")
        case .video: Icon(name: "film-strip", fallback: "film.fill")
        case .image: Icon(name: "image", fallback: "photo.fill")
        case .audio: Icon(name: "music-notes", fallback: "music.note")
        case .archive: Icon(name: "archive", fallback: "archivebox.fill")
        case .application: Icon(name: "app-window", fallback: "app.badge.fill")
        case .document: Icon(name: "file-text", fallback: "doc.text.fill")
        case .developer: Icon(name: "code", fallback: "hammer.fill")
        case .other: Icon(name: "file-dashed", fallback: "questionmark.square.dashed")
        }
    }

    var color: Color {
        switch self {
        case .folder: Color(red: 0.36, green: 0.56, blue: 0.86)
        case .video: Color(red: 0.93, green: 0.34, blue: 0.40)
        case .image: Color(red: 0.98, green: 0.62, blue: 0.20)
        case .audio: Color(red: 0.88, green: 0.36, blue: 0.78)
        case .archive: Color(red: 0.62, green: 0.48, blue: 0.36)
        case .application: Color(red: 0.34, green: 0.46, blue: 0.95)
        case .document: Color(red: 0.25, green: 0.72, blue: 0.56)
        case .developer: Color(red: 0.55, green: 0.42, blue: 0.90)
        case .other: Color(red: 0.56, green: 0.60, blue: 0.66)
        }
    }

    init(nameBytes: ArraySlice<UInt8>) {
        let suffixStart = max(nameBytes.startIndex, nameBytes.endIndex - 12)
        guard let dot = nameBytes[suffixStart...].lastIndex(of: UInt8(ascii: ".")),
              dot > nameBytes.startIndex else { self = .other; return }
        let extensionBytes = nameBytes[(dot + 1)...]
        if extensionBytes.count <= 9 {
            // A leading bit preserves the length; nine ASCII bytes fit in UInt64.
            var key: UInt64 = 1
            for b in extensionBytes {
                guard b < 128 else { self = .other; return }
                key = (key << 7) | UInt64(b >= 65 && b <= 90 ? b + 32 : b)
            }
            self = Self.byPackedExtension[key] ?? .other
            return
        }
        var ext = ""
        for b in extensionBytes {
            guard b < 128 else { self = .other; return }
            ext.unicodeScalars.append(Unicode.Scalar(b >= 65 && b <= 90 ? b + 32 : b))
        }
        self = Self.byExtension[ext] ?? .other
    }

    private static let byPackedExtension: [UInt64: FileCategory] = {
        var map: [UInt64: FileCategory] = [:]
        for (ext, category) in byExtension where ext.utf8.count <= 9 {
            var key: UInt64 = 1
            for byte in ext.utf8 { key = (key << 7) | UInt64(byte) }
            map[key] = category
        }
        return map
    }()

    private static let byExtension: [String: FileCategory] = {
        var map: [String: FileCategory] = [:]
        let groups: [(FileCategory, String)] = [
            (.video, "mov mp4 m4v mkv avi wmv flv webm mpg mpeg 3gp mts m2ts braw r3d mxf prproj fcpbundle"),
            (.image, "jpg jpeg png gif heic heif tif tiff bmp webp cr2 cr3 nef arw dng orf raf psd psb svg ico icns exr hdr avif"),
            (.audio, "mp3 m4a aac wav aif aiff flac ogg opus wma alac caf mid midi logicx band"),
            (.archive, "zip gz tgz bz2 xz 7z rar tar zst lz4 dmg iso img raw qcow2 vmdk vdi vhd vhdx asif sparseimage sparsebundle pkg mpkg xip cpgz ipsw sbi"),
            (.application, "app dylib so framework a o bundle plugin kext appex vst vst3 component exe dll msi bin"),
            (.document, "pdf doc docx xls xlsx ppt pptx key pages numbers txt rtf md csv epub mobi odt ods odp tex log"),
            (.developer, "c h cc cpp hpp m mm swift py js ts tsx jsx java kt go rs rb php json xml yaml yml toml sql db sqlite sqlite3 wasm pyc class jar node pack idx map pch pcm dSYM"),
        ]
        for (category, list) in groups {
            for ext in list.split(separator: " ") { map[ext.lowercased()] = category }
        }
        return map
    }()
}

enum Format {
    static func bytes(_ n: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: n), countStyle: .file)
    }

    static func count(_ n: Int) -> String {
        n.formatted(.number)
    }

    static func items(_ n: Int) -> String {
        n == 1 ? "1 item" : "\(n.formatted(.number)) items"
    }

    static func percent(_ part: UInt64, of whole: UInt64) -> String {
        guard whole > 0 else { return "—" }
        let value = Double(part) / Double(whole)
        if value > 0 && value < 0.001 { return "<0.1%" }
        return value.formatted(.percent.precision(.fractionLength(value < 0.1 ? 1 : 0)))
    }

    static func duration(_ seconds: TimeInterval) -> String {
        if seconds < 1 { return "\(Int((seconds * 1000).rounded())) ms" }
        if seconds < 60 { return String(format: "%.1f s", seconds) }
        return Duration.seconds(seconds).formatted(.units(allowed: [.minutes, .seconds], width: .abbreviated))
    }
}
