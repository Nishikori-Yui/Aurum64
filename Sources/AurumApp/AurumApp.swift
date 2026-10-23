// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        var snapshot = AurSnapshot(); aur_snapshot(&snapshot)
        if snapshot.state == AUR_STOPPED || snapshot.state == AUR_FAILED { _ = aur_wait(); return .terminateNow }
        aur_command(Int32(AUR_STOP))
        Task { @MainActor in
            repeat {
                try? await Task.sleep(for: .milliseconds(50))
                aur_snapshot(&snapshot)
            } while snapshot.state != AUR_STOPPED && snapshot.state != AUR_FAILED
            _ = aur_wait(); sender.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }
}

@main
struct AurumApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = EmulatorModel()
    var body: some Scene {
        Window("Aurum64", id: "main") {
            ContentView(model: model)
                .onAppear { aur_set_muted(UserDefaults.standard.bool(forKey: "muted") ? 1 : 0) }
        }
        .defaultSize(width: 960, height: 760)
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Open ROM...", action: model.chooseROM).keyboardShortcut("o").disabled(model.active)
            }
            CommandMenu("Emulation") {
                Button(model.paused ? "Resume" : "Pause", action: model.togglePause).keyboardShortcut("p").disabled(!model.canControl)
                Button("Stop", action: model.stop).disabled(!model.active)
                Divider()
                Button("Save States (Not Yet Available)") {}.disabled(true)
                Button("Reset") { aur_command(Int32(AUR_RESET)) }.disabled(!model.canControl)
            }
        }
        Settings { SettingsView() }
    }
}
