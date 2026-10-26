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

// Test-only entry points call the pinned core's actual coordinate and fetch
// functions. No coordinate formulas are duplicated in the adapter.
void n64video_coordinate_probe_init(void) {
    tcoord_init_lut();
    tmem_init_lut();
    for (unsigned i = 0; i < 4096; ++i)
        state[0].tmem[i] = (uint8_t)(i * 73u + (i >> 3) * 19u + 11u);
}

void n64video_coordinate_probe(const uint32_t* input, uint32_t* output) {
    struct rdp_state* wstate = &state[0];
    const unsigned tilenum = (input[1] >> 24) & 7;
    rdp_set_tile(wstate, input);
    rdp_set_tile_size(wstate, input + 2);
    struct tile* tile = &wstate->tile[tilenum];
    int32_t s = (int32_t)input[4], t = (int32_t)input[5], maxs, maxt;
    tcshift_cycle(tile, &s, &t, &maxs, &maxt);
    s = TRELATIVE(s, tile->sl);
    t = TRELATIVE(t, tile->tl);
    tcclamp_cycle_light(tile, &s, &t, maxs, maxt);
    tcmask(tile, &s, &t);
    output[0] = (uint32_t)s;
    output[1] = (uint32_t)t;
    struct color texel;
    fetch_texel(wstate, &texel, s, t, tilenum);
    output[7] = (uint32_t)texel.r;
    output[8] = (uint32_t)texel.g;
    output[9] = (uint32_t)texel.b;
    output[10] = (uint32_t)texel.a;
    int32_t s0 = (int32_t)input[4], s1, s2, s3;
    t = (int32_t)input[5];
    tc_pipeline_copy(tile, &s0, &s1, &s2, &s3, &t);
    output[2] = (uint32_t)s0; output[3] = (uint32_t)s1;
    output[4] = (uint32_t)s2; output[5] = (uint32_t)s3;
    output[6] = (uint32_t)t;
}
