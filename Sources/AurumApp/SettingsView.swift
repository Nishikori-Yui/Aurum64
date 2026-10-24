// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SettingsView: View {
    @AppStorage("showHUD") private var showHUD = true
    @AppStorage("linearFiltering") private var linearFiltering = false
    @AppStorage("muted") private var muted = false
    var body: some View {
        Form {
            Section("Video") {
                Toggle("Show performance counters", isOn: $showHUD)
                Toggle("Smooth display scaling", isOn: $linearFiltering)
                Text("Display scaling does not change the emulated resolution. Accurate VI filters and high-resolution rendering are not implemented.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Audio") { Toggle("Mute audio", isOn: $muted).onChange(of: muted) { aur_set_muted(muted ? 1 : 0) } }
            Section("Controls") {
                LabeledContent("Analog stick", value: "Arrow keys")
                LabeledContent("A / B / Z / Start", value: "X / Z / C / Return")
                LabeledContent("L / R", value: "Q / E")
                LabeledContent("C buttons", value: "I / K / J / L")
                LabeledContent("D-pad", value: "W / S / A / D")
                Text("Click the video to focus keyboard input. The first connected controller is player 1. Its right stick controls the C buttons.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }.formStyle(.grouped).frame(width: 480, height: 480)
    }
}
