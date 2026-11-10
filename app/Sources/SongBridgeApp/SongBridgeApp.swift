import AppKit
import SwiftUI

// "Re-ARMed Song Bridge": puts the mod into RS Re-ARMed's mods folders, brings it up to date, takes it out again.
@main
struct SongBridgeApp: App {
    @NSApplicationDelegateAdaptor private var delegate: AppDelegate
    @StateObject private var installer = Installer()

    var body: some Scene {
        Window("Re-ARMed Song Bridge", id: "main") {
            ContentView().environmentObject(installer)
        }
        .windowResizability(.contentSize)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Run from `swift run` (no app bundle) as a regular app with a Dock icon and a key window
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

struct ContentView: View {
    @EnvironmentObject var installer: Installer

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Song Bridge").font(.title2.weight(.semibold))
            Text("Each edition of RS Re-ARMed plays the other platform's songs, and feedpak song packages (experimental), "
                + "straight from its dlc folder. Needs RS Re-ARMed 1.14.0 or later.")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary)
            Divider()
            HStack(alignment: .firstTextBaseline) {
                Image(systemName: icon).foregroundStyle(tint)
                VStack(alignment: .leading, spacing: 4) {
                    Text(status)
                    if !installer.editionNames.isEmpty {
                        Text("Found: " + installer.editionNames.joined(separator: ", ")).font(.callout).foregroundStyle(.secondary)
                    }
                }
            }
            if let message = installer.message {
                Text(message).font(.callout).fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                if let label = installLabel {
                    Button(label) { installer.install() }.keyboardShortcut(.defaultAction)
                }
                if canRemove {
                    Button("Remove") { installer.uninstall() }
                }
                Spacer()
                if let songs = installer.songsFolder {
                    Button("Show Songs Folder") { NSWorkspace.shared.open(songs) }
                        .help("Put .psarc archives and .feedpak packages here, or in a folder inside it.")
                }
            }
        }
        .padding(20)
        .frame(width: 520)
        .onAppear { installer.refresh() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            let message = installer.message
            installer.refresh()
            if message != nil { /* a result just shown stays until the next action */ }
        }
    }

    private var status: String {
        switch installer.state {
        case .noRearmed: return "RS Re-ARMed is not installed (no game in ~/Library/Application Support/RS Re-ARMed)."
        case .notInstalled: return "Not installed." + version(" This app has ")
        case .installed(let v): return "Installed, version \(v)."
        case .updateAvailable(let v): return "Version \(v) is installed." + version(" This app has ")
        case .newerInstalled(let v): return "A newer version (\(v)) is installed than this app has." + version(" (", ")")
        case .noBundledLibrary: return "This app was built without the mod."
        }
    }

    private func version(_ before: String, _ after: String = ".") -> String {
        installer.bundledVersion.map { before + $0 + after } ?? ""
    }

    private var icon: String {
        switch installer.state {
        case .installed: return "checkmark.circle.fill"
        case .updateAvailable, .notInstalled: return "arrow.down.circle.fill"
        default: return "exclamationmark.triangle.fill"
        }
    }

    private var tint: Color {
        switch installer.state {
        case .installed: return .green
        case .updateAvailable, .notInstalled: return .accentColor
        default: return .orange
        }
    }

    private var installLabel: String? {
        switch installer.state {
        case .notInstalled: return "Install"
        case .updateAvailable: return "Update"
        default: return nil
        }
    }

    private var canRemove: Bool {
        switch installer.state {
        case .installed, .updateAvailable, .newerInstalled: return true
        default: return false
        }
    }
}
