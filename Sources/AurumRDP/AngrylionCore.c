// SPDX-License-Identifier: GPL-2.0-or-later
#include <core/n64video.c>

const uint8_t* n64video_tmem(void) {
    return state[0].tmem;
}

const uint8_t* n64video_hidden_rdram(void) {
    return rdram_hidden;
}

void n64video_set_vi_mode(int mode) {
    config.vi.mode = (enum vi_mode)mode;
}
