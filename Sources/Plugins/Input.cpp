// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common.hpp"
AUR_PLUGIN(M64PLUGIN_INPUT, 0x020100, "Aurum Native Input")
extern "C" {
EXPORT int CALL RomOpen() { return 1; }
EXPORT void CALL RomClosed() { aur_set_buttons(0); }
EXPORT void CALL InitiateControllers(CONTROL_INFO info) {
    for (int i = 0; i < 4; ++i) info.Controls[i] = {i == 0, 0, i == 0 ? PLUGIN_MEMPAK : PLUGIN_NONE, CONT_TYPE_STANDARD};
}
EXPORT void CALL GetKeys(int control, BUTTONS* keys) { keys->Value = control == 0 ? aur_get_buttons() : 0; }
EXPORT void CALL ControllerCommand(int, unsigned char*) {}
EXPORT void CALL ReadController(int, unsigned char*) {}
EXPORT void CALL SDL_KeyDown(int, int) {}
EXPORT void CALL SDL_KeyUp(int, int) {}
}
