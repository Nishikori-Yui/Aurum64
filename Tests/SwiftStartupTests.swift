// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

private struct TestFailure: Error, CustomStringConvertible {
    let description: String
}

private func require(_ condition: Bool, _ message: String) throws {
    if !condition { throw TestFailure(description: message) }
}

@main
struct SwiftStartupTests {
    static func main() throws {
        try require(CommandLine.arguments.count == 3, "Usage: aurum-swift-startup-tests ROM TestDirectory")
        let source = URL(filePath: CommandLine.arguments[1])
        let root = URL(filePath: CommandLine.arguments[2], directoryHint: .isDirectory)
            .appending(path: "Swift temporary paths \u{5b58}\u{6863}", directoryHint: .isDirectory)
        guard let resources = Bundle.main.resourceURL,
              let frameworks = Bundle.main.privateFrameworksURL else {
            throw TestFailure(description: "Run this test inside the relocated test app bundle.")
        }

        for iteration in 0..<8 {
            let directory = root.appending(path: "session \(iteration)", directoryHint: .isDirectory)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let rom = directory.appending(path: "Original test ROM.z64")
            try FileManager.default.copyItem(at: source, to: rom)
            let userData = directory.appending(path: "Application Support/Aurum64", directoryHint: .isDirectory)

            // Match EmulatorModel.start: each URL.path becomes a temporary C string
            // valid only during this call, not until the worker uses it later.
            let accepted = aur_start(rom.path, frameworks.path, resources.path, userData.path, 4)
            try require(accepted == 1, "Swift startup was rejected.")
            // Reuse allocator storage after the temporary C strings have expired.
            let scratch = (0..<256).map { index in
                [UInt8](repeating: UInt8(index), count: 64 + index)
            }
            let result = withExtendedLifetime(scratch) { aur_wait() }
            var snapshot = AurSnapshot()
            aur_snapshot(&snapshot)
            let message = withUnsafeBytes(of: snapshot.message) {
                String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self)
            }
            try require(result == 0 && snapshot.state == AUR_STOPPED, "Swift startup failed: \(message)")
            try require(snapshot.frames >= 4 && snapshot.width == 320 && snapshot.height == 240,
                        "Swift startup did not produce the expected video output.")
            try require(snapshot.rdpCommands == 6 && snapshot.unsupportedCommands == 0 && snapshot.audioFrames == 1024,
                        "Swift startup did not execute the expected ROM/RDP/audio sequence.")

            var pixels = [UInt32](repeating: 0, count: 320 * 240)
            var width: UInt32 = 0, height: UInt32 = 0
            let count = pixels.withUnsafeMutableBufferPointer { buffer in
                aur_copy_frame(buffer.baseAddress, buffer.count, &width, &height)
            }
            try require(count == pixels.count && width == 320 && height == 240, "Invalid Swift frame copy.")
            try require(pixels.enumerated().allSatisfy { index, pixel in
                pixel == (index < 2 ? 0xff00ff00 : 0xff2963ef)
            }, "Swift startup framebuffer differs from the original ROM reference.")
            for name in ["Saves", "States"] {
                var isDirectory: ObjCBool = false
                try require(FileManager.default.fileExists(atPath: userData.appending(path: name).path,
                                                          isDirectory: &isDirectory) && isDirectory.boolValue,
                            "\(name) was not created under the requested user-data directory.")
            }
            let config = try String(contentsOf: userData.appending(path: "mupen64plus.cfg"), encoding: .utf8)
            try require(config.contains(userData.appending(path: "Saves").path), "Core save path was corrupted.")
            try require(config.contains(userData.appending(path: "States").path), "Core state path was corrupted.")
        }
        print("PASS: 8 optimized Swift startups with temporary URL paths, spaces/Unicode, exact video and isolated save directories")
    }
}
