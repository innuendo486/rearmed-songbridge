import XCTest
@testable import SongBridgeApp

// The installer on a made-up RS Re-ARMed folder: what it finds, installs, updates and removes.
@MainActor
final class InstallerTests: XCTestCase {
    private var root: URL!
    private var home: URL { root.appendingPathComponent("home") }
    private let fm = FileManager.default

    override func setUp() async throws {
        root = fm.temporaryDirectory.appendingPathComponent("songbridge-app-\(UUID().uuidString)")
        try fm.createDirectory(at: home, withIntermediateDirectories: true)
    }

    override func tearDown() async throws { try? fm.removeItem(at: root) }

    private func library(_ version: String?, _ name: String = "lib") throws -> URL {
        let url = root.appendingPathComponent(name + ".dylib")
        var data = Data([0xcf, 0xfa, 0xed, 0xfe]) + Data(repeating: 7, count: 100)
        if let version { data += Data("songbridge-version:\(version)".utf8) + Data([0]) + Data(repeating: 9, count: 20) }
        try data.write(to: url)
        return url
    }

    private func edition(_ path: String) throws {
        try fm.createDirectory(at: home.appendingPathComponent(path).appendingPathComponent("game/dlc"), withIntermediateDirectories: true)
    }

    private func installed(_ path: String) -> URL { home.appendingPathComponent(path).appendingPathComponent("mods/songbridge.dylib") }

    func testNoGame() throws {
        let i = Installer(bundled: try library("1.2.3"), home: home, gameIsRunning: { false })
        i.refresh()
        XCTAssertEqual(i.state, .noRearmed)
        XCTAssertNil(i.songsFolder)
    }

    func testInstallIntoBothEditionsThenRemove() throws {
        try edition("")
        try edition("learn-play")
        let lib = try library("1.2.3")
        let i = Installer(bundled: lib, home: home, gameIsRunning: { false })
        i.refresh()
        XCTAssertEqual(i.state, .notInstalled)
        XCTAssertEqual(i.editionNames, ["Mac edition", "Learn & Play"])
        i.install()
        XCTAssertEqual(i.state, .installed("1.2.3"))
        XCTAssertTrue(fm.contentsEqual(atPath: installed("").path, andPath: lib.path))
        XCTAssertTrue(fm.contentsEqual(atPath: installed("learn-play").path, andPath: lib.path))
        XCTAssertFalse(fm.fileExists(atPath: installed("").path + ".new"))
        XCTAssertEqual(i.songsFolder?.path, home.appendingPathComponent("game/dlc").path)
        i.uninstall()
        XCTAssertEqual(i.state, .notInstalled)
        XCTAssertFalse(fm.fileExists(atPath: installed("").path))
        XCTAssertTrue(fm.fileExists(atPath: home.appendingPathComponent("game/dlc").path))  // the songs stay
    }

    func testOnlyLearnAndPlay() throws {
        try edition("learn-play")
        let i = Installer(bundled: try library("1.0.0"), home: home, gameIsRunning: { false })
        i.install()
        XCTAssertEqual(i.state, .installed("1.0.0"))
        XCTAssertFalse(fm.fileExists(atPath: installed("").path))
        XCTAssertEqual(i.songsFolder?.path, home.appendingPathComponent("learn-play/game/dlc").path)
    }

    func testUpdateAndNewer() throws {
        try edition("")
        try edition("learn-play")
        let old = Installer(bundled: try library("1.0.0", "old"), home: home, gameIsRunning: { false })
        old.install()
        let new = Installer(bundled: try library("1.10.0", "new"), home: home, gameIsRunning: { false })
        new.refresh()
        XCTAssertEqual(new.state, .updateAvailable("1.0.0"))  // 1.10.0 is newer than 1.0.0, by numbers
        new.install()
        XCTAssertEqual(new.state, .installed("1.10.0"))
        old.refresh()
        XCTAssertEqual(old.state, .newerInstalled("1.10.0"))  // never put the older over it
    }

    func testOneEditionLacksIt() throws {
        try edition("")
        let lib = try library("2.0.0")
        let i = Installer(bundled: lib, home: home, gameIsRunning: { false })
        i.install()
        try edition("learn-play")  // installed afterwards
        i.refresh()
        XCTAssertEqual(i.state, .updateAvailable("2.0.0"))
        i.install()
        XCTAssertEqual(i.state, .installed("2.0.0"))
    }

    func testAModFromBeforeVersions() throws {
        try edition("")
        try fm.createDirectory(at: installed("").deletingLastPathComponent(), withIntermediateDirectories: true)
        try fm.copyItem(at: try library(nil, "plain"), to: installed(""))
        let i = Installer(bundled: try library("0.1.0"), home: home, gameIsRunning: { false })
        i.refresh()
        XCTAssertEqual(i.state, .updateAvailable("unknown"))
    }

    func testNotWhileTheGameRuns() throws {
        try edition("")
        let i = Installer(bundled: try library("1.0.0"), home: home, gameIsRunning: { true })
        i.install()
        XCTAssertEqual(i.state, .notInstalled)
        XCTAssertEqual(i.message, "Quit RS Re-ARMed first.")
    }

    func testBuiltWithoutTheMod() throws {
        try edition("")
        let i = Installer(bundled: nil, home: home, gameIsRunning: { false })
        i.refresh()
        XCTAssertEqual(i.state, .noBundledLibrary)
    }
}
