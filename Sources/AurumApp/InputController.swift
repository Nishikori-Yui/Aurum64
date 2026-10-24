// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import GameController

@MainActor
final class InputController {
    private var keys: Set<UInt16> = []
    func key(_ code: UInt16, pressed: Bool) {
        if pressed { keys.insert(code) } else { keys.remove(code) }
        poll()
    }
    func clear() { keys.removeAll(); aur_set_buttons(0) }
    func poll() {
        guard NSApp.isActive else { clear(); return }
        var buttons: UInt32 = 0
        let mapping: [UInt16: UInt32] = [7: 0x80, 6: 0x40, 8: 0x20, 36: 0x10,
            12: 0x2000, 14: 0x1000, 34: 0x800, 40: 0x400, 38: 0x200, 37: 0x100,
            13: 8, 1: 4, 0: 2, 2: 1]
        for key in keys { buttons |= mapping[key, default: 0] }
        var x: Float = (keys.contains(124) ? 1 : 0) - (keys.contains(123) ? 1 : 0)
        var y: Float = (keys.contains(126) ? 1 : 0) - (keys.contains(125) ? 1 : 0)
        if let pad = GCController.controllers().first?.extendedGamepad {
            if abs(pad.leftThumbstick.xAxis.value) > 0.15 { x = pad.leftThumbstick.xAxis.value }
            if abs(pad.leftThumbstick.yAxis.value) > 0.15 { y = pad.leftThumbstick.yAxis.value }
            for (pressed, bit): (Bool, UInt32) in [
                (pad.buttonA.isPressed, 0x80), (pad.buttonX.isPressed, 0x40),
                (pad.leftTrigger.isPressed, 0x20), (pad.buttonMenu.isPressed, 0x10),
                (pad.leftShoulder.isPressed, 0x2000), (pad.rightShoulder.isPressed, 0x1000),
                (pad.dpad.up.isPressed, 8), (pad.dpad.down.isPressed, 4),
                (pad.dpad.left.isPressed, 2), (pad.dpad.right.isPressed, 1),
                (pad.rightThumbstick.up.isPressed, 0x800), (pad.rightThumbstick.down.isPressed, 0x400),
                (pad.rightThumbstick.left.isPressed, 0x200), (pad.rightThumbstick.right.isPressed, 0x100)
            ] where pressed { buttons |= bit }
        }
        buttons |= UInt32(UInt8(bitPattern: Int8(x * 80))) << 16
        buttons |= UInt32(UInt8(bitPattern: Int8(y * 80))) << 24
        aur_set_buttons(buttons)
    }
}
