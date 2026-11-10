import Foundation

// Installs the mod the app carries (Contents/Resources/songbridge.dylib, put there by build-app.sh) into RS Re-ARMed's
// mods folders: <home>/mods for the Mac edition, <home>/learn-play/mods for Learn & Play, the same file in both. RS
// Re-ARMed loads what is there when the game starts. <home> is ~/Library/Application Support/RS Re-ARMed; an edition is
// installed when its folder has a game/.
@MainActor
final class Installer: ObservableObject {
    enum State: Equatable {
        case noRearmed                // neither edition is installed
        case notInstalled
        case installed(String)        // up to date (version)
        case updateAvailable(String)  // another version is installed, or an edition lacks the mod
        case newerInstalled(String)   // a newer mod than the app's (a newer release or a local build)
        case noBundledLibrary         // the app was built without it (swift run)
    }

    @Published private(set) var state: State = .notInstalled
    @Published private(set) var message: String?
    @Published private(set) var editionNames: [String] = []

    static let fileName = "songbridge.dylib"
    private static let versionMarker = Data("songbridge-version:".utf8)

    let bundled: URL?
    let home: URL
    private let gameIsRunning: () -> Bool

    init(bundled: URL? = Bundle.main.url(forResource: "songbridge", withExtension: "dylib"),
         home: URL = Installer.defaultHome,
         gameIsRunning: @escaping () -> Bool = Installer.gameRunning) {
        self.bundled = bundled
        self.home = home
        self.gameIsRunning = gameIsRunning
    }

    nonisolated static var defaultHome: URL {
        URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support/RS Re-ARMed", isDirectory: true)
    }

    var bundledVersion: String? { bundled.flatMap(Self.version(of:)) }

    // The installed editions: (name, folder), each folder with its game/ and mods/
    private var editions: [(name: String, folder: URL)] {
        [("Mac edition", home), ("Learn & Play", home.appendingPathComponent("learn-play"))].filter {
            FileManager.default.fileExists(atPath: $0.1.appendingPathComponent("game").path)
        }
    }

    private func installedURL(_ edition: URL) -> URL {
        edition.appendingPathComponent("mods").appendingPathComponent(Self.fileName)
    }

    // Where songs go: the Mac edition's dlc when it is there (Learn & Play sees it through a link, when one is made),
    // else Learn & Play's
    var songsFolder: URL? {
        editions.first.map { $0.folder.appendingPathComponent("game/dlc", isDirectory: true) }
    }

    func refresh() {
        message = nil
        let fm = FileManager.default
        editionNames = editions.map(\.name)
        guard !editions.isEmpty else { state = .noRearmed; return }
        guard let bundled, let bundledVersion else { state = .noBundledLibrary; return }
        let installed = editions.map { installedURL($0.folder) }.filter { fm.fileExists(atPath: $0.path) }
        guard let first = installed.first else { state = .notInstalled; return }
        if installed.count == editions.count, installed.allSatisfy({ fm.contentsEqual(atPath: $0.path, andPath: bundled.path) }) {
            state = .installed(bundledVersion)
            return
        }
        let versions = installed.map { Self.version(of: $0) ?? "unknown" }
        // Never offered as an update, in any edition: it would put the older mod over the newer
        if let newer = versions.first(where: { Self.isNewer($0, than: bundledVersion) }) {
            state = .newerInstalled(newer)
        } else {
            state = .updateAvailable(Self.version(of: first) ?? "unknown")
        }
    }

    func install() {
        guard let bundled else { return }
        guard !gameIsRunning() else { message = "Quit RS Re-ARMed first."; return }
        let fm = FileManager.default
        var result: String
        do {
            for edition in editions {
                let target = installedURL(edition.folder)
                let dir = target.deletingLastPathComponent()
                let temp = dir.appendingPathComponent(Self.fileName + ".new")
                try fm.createDirectory(at: dir, withIntermediateDirectories: true)
                try? fm.removeItem(at: temp)
                try fm.copyItem(at: bundled, to: temp)
                // A file copied out of a downloaded app keeps its quarantine flag, and the game would not load it
                removexattr(temp.path, "com.apple.quarantine", 0)
                // A new file renamed over the old one: overwriting a library in place leaves macOS with the old code
                // signature cached for the file
                if fm.fileExists(atPath: target.path) {
                    _ = try fm.replaceItemAt(target, withItemAt: temp)
                } else {
                    try fm.moveItem(at: temp, to: target)
                }
            }
            result = "Installed in " + editions.map(\.name).joined(separator: " and ") + ". Start RS Re-ARMed to use it."
        } catch {
            result = "Install failed: \(error.localizedDescription)"
        }
        refresh()
        message = result
    }

    func uninstall() {
        guard !gameIsRunning() else { message = "Quit RS Re-ARMed first."; return }
        let fm = FileManager.default
        var result: String
        do {
            for edition in editions where fm.fileExists(atPath: installedURL(edition.folder).path) {
                try fm.removeItem(at: installedURL(edition.folder))
            }
            result = "Removed. RS Re-ARMed starts without the mod; your songs are untouched."
        } catch {
            result = "Remove failed: \(error.localizedDescription)"
        }
        refresh()
        message = result
    }

    // Versions are major.minor.patch (VERSION); one that doesn't parse is never newer
    static func isNewer(_ version: String, than other: String) -> Bool {
        guard version.first?.isNumber == true else { return false }
        return version.compare(other, options: .numeric) == .orderedDescending
    }

    // The version tag the mod carries (sb_version_tag in src/songbridge.c); a mod from before it had one has none
    nonisolated static func version(of url: URL) -> String? {
        guard let data = try? Data(contentsOf: url, options: .mappedIfSafe),
              let r = data.range(of: versionMarker) else { return nil }
        let bytes = data[r.upperBound...].prefix(32).prefix { $0 != 0 }
        return String(decoding: bytes, as: UTF8.self)
    }

    nonisolated static func gameRunning() -> Bool {
        ["Rocksmith2014", "rs2014_lp"].contains { name in
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
            p.arguments = ["-x", name]
            p.standardOutput = FileHandle.nullDevice
            guard (try? p.run()) != nil else { return false }  // waiting for a process that never ran would raise
            p.waitUntilExit()
            return p.terminationStatus == 0
        }
    }
}
