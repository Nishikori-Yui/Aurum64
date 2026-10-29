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

// Execute the pinned implementation and export its real intermediate state.
// No edgewalker or interpolation arithmetic is reimplemented in this adapter.
void n64video_attribute_probe(const uint32_t* input, uint32_t* output) {
    struct rdp_state* wstate = &state[0];
    const uint32_t image[] = {0xff10001f, 0x100000};
    const uint32_t modes[] = {0xef0000f0, 0};
    const uint32_t combine[] = {0xfcffffff, 0xfffdf6fb};
    const uint32_t primitive[] = {0xfa000000, 0x804020ff};
    rdp_set_color_image(wstate, image);
    rdp_set_other_modes(wstate, modes);
    rdp_set_combine(wstate, combine);
    rdp_set_prim_color(wstate, primitive);
    rdp_set_scissor(wstate, input + 44);
    memset(wstate->span, 0, sizeof(wstate->span));
    switch ((input[0] >> 24) & 63) {
        case 8: rdp_tri_noshade(wstate, input); break;
        case 9: rdp_tri_noshade_z(wstate, input); break;
        case 10: rdp_tri_tex(wstate, input); break;
        case 11: rdp_tri_tex_z(wstate, input); break;
        case 12: rdp_tri_shade(wstate, input); break;
        case 13: rdp_tri_shade_z(wstate, input); break;
        case 14: rdp_tri_texshade(wstate, input); break;
        case 15: rdp_tri_texshade_z(wstate, input); break;
    }
    const struct span* row = &wstate->span[input[46]];
    output[0] = row->validline;
    output[1] = row->unscrx;
    output[2] = row->rx;
    output[3] = row->r; output[4] = row->g; output[5] = row->b; output[6] = row->a;
    output[7] = row->s; output[8] = row->t; output[9] = row->w; output[10] = row->z;
    output[11] = wstate->spans_dr; output[12] = wstate->spans_dg;
    output[13] = wstate->spans_db; output[14] = wstate->spans_da;
    output[15] = wstate->spans_ds; output[16] = wstate->spans_dt;
    output[17] = wstate->spans_dw; output[18] = wstate->spans_dz;
    output[19] = wstate->spans_dsdy; output[20] = wstate->spans_dtdy;
    output[21] = wstate->spans_dwdy;
    int32_t s, t, z = ((uint32_t)row->z >> 10);
    tcdiv_nopersp(row->s >> 16, row->t >> 16, row->w >> 16, &s, &t);
    z_correct(wstate, 0, 0, &z, 8);
    output[22] = s; output[23] = t; output[24] = z;
}

void n64video_perspective_probe_init(void) { tcoord_init_lut(); }
void n64video_perspective_probe(const uint32_t* input, uint32_t* output) {
    int32_t s, t;
    if (input[0]) tcdiv_persp(input[1], input[2], input[3], &s, &t);
    else tcdiv_nopersp(input[1], input[2], input[3], &s, &t);
    output[0] = s; output[1] = t;
}
