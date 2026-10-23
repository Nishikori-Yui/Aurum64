// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { AUR_STOPPED, AUR_STARTING, AUR_RUNNING, AUR_PAUSED, AUR_STOPPING, AUR_FAILED };
enum { AUR_STOP, AUR_PAUSE, AUR_RESUME, AUR_SAVE, AUR_LOAD, AUR_RESET };
typedef struct {
    int state;
    uint32_t width, height;
    uint64_t frames, rdpCommands, unsupportedCommands, audioFrames;
    char message[512];
} AurSnapshot;
enum { AUR_DIAGNOSTICS_VERSION = 1 };
typedef struct {
    uint32_t size;
    uint32_t version;
    int32_t state;
    uint32_t width, height;
    uint64_t frames, rdpCommands, unsupportedCommands, audioFrames;
} AurDiagnostics;
// Singleton core. Worker owns all upstream calls; UI uses a command mailbox.
// All paths are copied before returning; callers need not retain their buffers.
// Returns 0 for null/empty paths or an active session, without changing that session.
int aur_start(const char* rom, const char* frameworks, const char* resources,
              const char* userData, uint64_t frameLimit);
void aur_command(int command);
void aur_snapshot(AurSnapshot* result);
// Versioned diagnostics ABI. The caller supplies the allocated size so newer
// fields cannot silently overwrite an older consumer's buffer.
int aur_diagnostics(AurDiagnostics* result, size_t size);
size_t aur_copy_frame(uint32_t* output, size_t capacity, uint32_t* width, uint32_t* height);
void aur_set_buttons(uint32_t buttons);
uint32_t aur_get_buttons(void);
void aur_set_muted(int muted);
int aur_get_muted(void);
int aur_wait(void);
// Plugin-to-host interface; called only from the emulation worker.
void aur_publish_frame(const uint32_t* pixels, uint32_t width, uint32_t height,
                       uint64_t commands, uint64_t unsupported);
void aur_video_tick(void);
void aur_audio_frames(uint32_t frames);
void aur_set_rdram_size(uint32_t bytes);
uint32_t aur_get_rdram_size(void);
void aur_fail(const char* message);
const char* aur_shader_path(void);
#ifdef __cplusplus
}
#endif
