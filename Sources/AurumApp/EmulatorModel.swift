// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import Observation
import UniformTypeIdentifiers

@MainActor @Observable
final class EmulatorModel {
    var state = Int32(AUR_STOPPED)
    var title = "Aurum64"
    var message = ""
    var frames: UInt64 = 0
    var commands: UInt64 = 0
    var unsupported: UInt64 = 0
    var viRate = 0.0
    private var previousFrames: UInt64 = 0
    private var previousTime = Date()
    var active: Bool { ![Int32(AUR_STOPPED), Int32(AUR_FAILED)].contains(state) }
    var paused: Bool { state == AUR_PAUSED }
    var canControl: Bool { state == AUR_RUNNING || state == AUR_PAUSED }

    func chooseROM() {
        guard !active else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = ["z64", "n64", "v64"].compactMap { UTType(filenameExtension: $0) }
        panel.allowsMultipleSelection = false
        panel.message = "Open an uncompressed ROM you are authorized to use."
        if panel.runModal() == .OK, let url = panel.url { start(url) }
    }

    func start(_ url: URL) {
        guard !active else { return }
        guard let resources = Bundle.main.resourceURL,
              let frameworks = Bundle.main.privateFrameworksURL else {
            message = "Run Aurum64.app from the build output."; return
        }
        let data = URL.applicationSupportDirectory.appending(path: "Aurum64", directoryHint: .isDirectory)
        if aur_start(url.path, frameworks.path, resources.path, data.path, 0) == 0 {
            message = "The previous session is still stopping."; return
        }
        title = url.deletingPathExtension().lastPathComponent
        state = Int32(AUR_STARTING); message = ""; frames = 0; previousFrames = 0
        previousTime = Date()
    }

    func refresh() {
        var snapshot = AurSnapshot()
        aur_snapshot(&snapshot)
        state = snapshot.state; frames = snapshot.frames
        commands = snapshot.rdpCommands; unsupported = snapshot.unsupportedCommands
        message = withUnsafeBytes(of: snapshot.message) { bytes in
            String(decoding: bytes.prefix(while: { $0 != 0 }), as: UTF8.self)
        }
        let now = Date()
        let elapsed = now.timeIntervalSince(previousTime)
        if elapsed >= 0.5 {
            viRate = Double(frames >= previousFrames ? frames - previousFrames : 0) / elapsed
            previousFrames = frames; previousTime = now
        }
    }

    func togglePause() { aur_command(paused ? Int32(AUR_RESUME) : Int32(AUR_PAUSE)) }
    func stop() { aur_command(Int32(AUR_STOP)) }
}
