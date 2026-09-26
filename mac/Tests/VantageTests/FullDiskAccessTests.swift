import Foundation
import Testing
@testable import Vantage

@Suite struct FullDiskAccessTests {
    @Test func probeDistinguishesAccessDenialAndMissingFolders() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("vantage-access-\(UUID().uuidString)")
        let readable = root.appendingPathComponent("readable")
        let locked = root.appendingPathComponent("locked")
        try FileManager.default.createDirectory(at: readable, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: locked, withIntermediateDirectories: true)
        try FileManager.default.setAttributes([.posixPermissions: 0], ofItemAtPath: locked.path)
        defer {
            try? FileManager.default.setAttributes([.posixPermissions: 0o700], ofItemAtPath: locked.path)
            try? FileManager.default.removeItem(at: root)
        }
        let missing = root.appendingPathComponent("missing").path

        #expect(FullDiskAccess.probe(readable.path) == .granted)
        #expect(FullDiskAccess.probe(missing) == nil)
        #expect(FullDiskAccess.check([missing]) == .unknown)
        // A missing folder is skipped rather than deciding the result.
        #expect(FullDiskAccess.check([missing, readable.path]) == .granted)
        if getuid() != 0 {
            #expect(FullDiskAccess.probe(locked.path) == .denied)
            #expect(FullDiskAccess.check([missing, locked.path, readable.path]) == .denied)
        }
    }

    @Test func guideOpensAtLaunchOnlyWhenItHasSomethingToSay() {
        typealias Guide = AccessGuide
        #expect(Guide.launchPage(progress: .notStarted, status: .denied) == .intro)
        #expect(Guide.launchPage(progress: .waiting, status: .denied) == .grant)
        // Relaunched after switching access on.
        #expect(Guide.launchPage(progress: .waiting, status: .granted) == .done)
        #expect(Guide.launchPage(progress: .notStarted, status: .granted) == nil)
        #expect(Guide.launchPage(progress: .skipped, status: .denied) == nil)
        #expect(Guide.launchPage(progress: .done, status: .denied) == nil)
        #expect(Guide.launchPage(progress: .notStarted, status: .unknown) == nil)
    }
}
