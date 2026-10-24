// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

extern "C" {
enum vi_mode_reference {
    reference_vi_color = 1,
    reference_vi_depth = 2,
    reference_vi_coverage = 3
};
const uint8_t* n64video_tmem(void);
const uint8_t* n64video_hidden_rdram(void);
void n64video_set_vi_mode(int mode);
void n64video_rdp_state_summary(uint8_t* output);
}
