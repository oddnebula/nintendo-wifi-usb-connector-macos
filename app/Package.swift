// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "WFCConnector",
    platforms: [.macOS(.v14)],
    targets: [
        .executableTarget(name: "WFCConnector", path: "Sources/WFCConnector")
    ]
)
