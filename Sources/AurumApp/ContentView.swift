// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct ContentView: View {
    @Bindable var model: EmulatorModel
    @AppStorage("showHUD") private var showHUD = true
    @AppStorage("linearFiltering") private var linearFiltering = false
    var body: some View {
        VStack(spacing: 0) {
            ZStack {
                MetalDisplay(linearFiltering: linearFiltering)
                if !model.active && model.frames == 0 {
                    ContentUnavailableView {
                        Label("Open a Nintendo 64 ROM", systemImage: "gamecontroller")
                    } description: {
                        Text("Experimental build. Graphics currently support fill rectangles and basic video output, not commercial game compatibility.")
                            .frame(maxWidth: 440)
                    } actions: {
                        Button("Open ROM...", action: model.chooseROM).buttonStyle(.borderedProminent)
                    }
                    .padding(32)
                    .background(.regularMaterial)
                }
                if model.paused {
                    Label("Paused", systemImage: "pause.fill").padding(12).background(.regularMaterial, in: .capsule)
                }
            }
            if !model.message.isEmpty {
                Label(model.message, systemImage: model.state == AUR_FAILED ? "exclamationmark.triangle" : "info.circle")
                    .font(.callout).textSelection(.enabled).padding(12).frame(maxWidth: .infinity, alignment: .leading)
            }
            if model.unsupported > 0 {
                Text("This ROM uses graphics commands that are not implemented. The picture is incomplete.")
                    .font(.callout).foregroundStyle(.orange).padding(8)
            }
            if showHUD {
                HStack(spacing: 16) {
                    Text("\(model.viRate, specifier: "%.1f") VI/s")
                    Text("RDP \(model.commands)")
                    Text("Unsupported \(model.unsupported)")
                    Spacer()
                    Text("Metal · LLE · Interpreter").foregroundStyle(.secondary)
                }
                .font(.caption.monospacedDigit()).padding(.horizontal, 16).padding(.vertical, 8)
            }
        }
        .frame(minWidth: 680, minHeight: 500)
        .navigationTitle(model.title)
        .toolbar {
            ToolbarItemGroup {
                Button("Open ROM", systemImage: "folder", action: model.chooseROM).disabled(model.active)
                Button(model.paused ? "Resume" : "Pause", systemImage: model.paused ? "play.fill" : "pause.fill", action: model.togglePause)
                    .disabled(!model.canControl)
                Button("Stop", systemImage: "stop.fill", action: model.stop).disabled(!model.active)
            }
        }
        .task {
            while !Task.isCancelled {
                model.refresh()
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
        .onOpenURL { model.start($0) }
    }
}
