// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "FPlusSearch",
    platforms: [.macOS(.v13)],
    products: [.library(name: "FPlusSearch", targets: ["FPlusSearch"])],
    targets: [.target(name: "FPlusSearch")]
)
