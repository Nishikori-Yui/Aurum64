// SPDX-License-Identifier: GPL-2.0-or-later
#include <core/n64video.c>

void n64video_rdp_state_summary(uint8_t* output) {
    uint32_t* words = (uint32_t*)output;
    const struct rdp_state* wstate = &state[0];
    unsigned index = 0;
    words[index++] = (uint32_t)wstate->fb_format;
    words[index++] = (uint32_t)wstate->fb_size;
    words[index++] = (uint32_t)wstate->fb_width;
    words[index++] = wstate->fb_address;
    words[index++] = wstate->fill_color;
    words[index++] = (uint32_t)wstate->clip.xh;
    words[index++] = (uint32_t)wstate->clip.yh;
    words[index++] = (uint32_t)wstate->clip.xl;
    words[index++] = (uint32_t)wstate->clip.yl;
    words[index++] = (uint32_t)wstate->other_modes.cycle_type;
    words[index++] = (uint32_t)wstate->other_modes.image_read_en;
    words[index++] = (uint32_t)wstate->other_modes.z_update_en;
    words[index++] = (uint32_t)wstate->other_modes.z_compare_en;
    words[index++] = (uint32_t)wstate->ti_format;
    words[index++] = (uint32_t)wstate->ti_size;
    words[index++] = (uint32_t)wstate->ti_width;
    words[index++] = wstate->ti_address;
    words[index++] = wstate->zb_address;
    for (unsigned tile = 0; tile < 8; ++tile) {
        words[index++] = (uint32_t)wstate->tile[tile].format;
        words[index++] = (uint32_t)wstate->tile[tile].size;
        words[index++] = (uint32_t)wstate->tile[tile].line;
        words[index++] = (uint32_t)wstate->tile[tile].tmem;
    }
    while (index < 64) words[index++] = 0;
    words[63] = 1;
}

const uint8_t* n64video_tmem(void) {
    return state[0].tmem;
}

const uint8_t* n64video_hidden_rdram(void) {
    return rdram_hidden;
}

void n64video_set_vi_mode(int mode) {
    config.vi.mode = (enum vi_mode)mode;
}
