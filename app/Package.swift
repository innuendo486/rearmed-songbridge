// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "SongBridgeApp",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(name: "SongBridgeApp", path: "Sources/SongBridgeApp"),
        .testTarget(name: "SongBridgeAppTests", dependencies: ["SongBridgeApp"], path: "Tests/SongBridgeAppTests"),
    ]
)
