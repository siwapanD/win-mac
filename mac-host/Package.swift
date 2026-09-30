// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "MacHost",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "mac-host", targets: ["MacHost"])
    ],
    targets: [
        .executableTarget(
            name: "MacHost",
            path: "Sources/MacHost"
        ),
        .testTarget(
            name: "MacHostTests",
            dependencies: ["MacHost"],
            path: "Tests/MacHostTests"
        )
    ]
)
