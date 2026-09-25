// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Vantage",
    platforms: [.macOS("26.0")],
    targets: [
        .executableTarget(
            name: "Vantage",
            path: "Sources/Vantage",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "VantageTests",
            dependencies: ["Vantage"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
