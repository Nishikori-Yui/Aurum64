// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common.hpp"
#include <AudioToolbox/AudioToolbox.h>
#include <array>
#include <atomic>
#include <cstring>
#include <algorithm>

AUR_PLUGIN(M64PLUGIN_AUDIO, 0x020000, "Aurum Core Audio")
namespace {
AUDIO_INFO info{};
AudioQueueRef queue = nullptr;
constexpr size_t capacity = 1 << 18;
std::array<int16_t, capacity> ring{};
std::atomic<uint64_t> readHead{0}, writeHead{0};
int rate = 32000, level = 100;
void callback(void*, AudioQueueRef q, AudioQueueBufferRef buffer) {
    auto* out = static_cast<int16_t*>(buffer->mAudioData);
    uint64_t read = readHead.load(std::memory_order_relaxed);
    uint64_t write = writeHead.load(std::memory_order_acquire);
    const size_t samples = buffer->mAudioDataBytesCapacity / 2;
    const bool mute = aur_get_muted();
    for (size_t i = 0; i < samples; ++i) {
        int16_t value = read < write ? ring[read++ % capacity] : 0;
        out[i] = mute ? 0 : value;
    }
    readHead.store(read, std::memory_order_release);
    buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(q, buffer, 0, nullptr);
}
void closeAudio() {
    if (queue) { AudioQueueStop(queue, true); AudioQueueDispose(queue, true); queue = nullptr; }
    readHead = writeHead = 0;
}
bool openAudio() {
    closeAudio();
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate; format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = 4; format.mFramesPerPacket = 1; format.mBytesPerFrame = 4;
    format.mChannelsPerFrame = 2; format.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&format, callback, nullptr, nullptr, nullptr, 0, &queue)) return false;
    for (int i = 0; i < 3; ++i) {
        AudioQueueBufferRef buffer;
        if (AudioQueueAllocateBuffer(queue, 2048, &buffer)) { closeAudio(); return false; }
        buffer->mAudioDataByteSize = 2048; std::memset(buffer->mAudioData, 0, 2048);
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }
    if (AudioQueueStart(queue, nullptr)) { closeAudio(); return false; }
    return true;
}
}
extern "C" {
EXPORT int CALL InitiateAudio(AUDIO_INFO value) { info = value; return 1; }
EXPORT int CALL RomOpen() { return 1; }
EXPORT void CALL RomClosed() { closeAudio(); }
EXPORT void CALL AiDacrateChanged(int system) {
    const uint32_t clocks[] = {48681812, 49656530, 48628316};
    rate = clocks[std::clamp(system, 0, 2)] / ((*info.AI_DACRATE_REG & 0x3fff) + 1);
    if (rate < 4000 || rate > 192000) { closeAudio(); return; }
    if (!openAudio()) aur_fail("Cannot start the Core Audio output device.");
}
EXPORT void CALL AiLenChanged() {
    if (!queue) return;
    uint32_t address = *info.AI_DRAM_ADDR_REG & 0x00fffff8;
    uint32_t length = *info.AI_LEN_REG & 0x0003fff8;
    const uint32_t memorySize = aur_get_rdram_size();
    if (address >= memorySize) return;
    length = std::min(length, memorySize - address);
    uint64_t write = writeHead.load(std::memory_order_relaxed);
    const uint64_t read = readHead.load(std::memory_order_acquire);
    for (uint32_t offset = 0; offset < length; offset += 4) {
        if (write - read + 2 > capacity) break; // Drop excess; never block the emulation thread.
        uint32_t sample; std::memcpy(&sample, info.RDRAM + address + offset, 4);
        ring[write++ % capacity] = int16_t(sample >> 16) * level / 100;
        ring[write++ % capacity] = int16_t(sample) * level / 100;
    }
    writeHead.store(write, std::memory_order_release); aur_audio_frames(length / 4);
}
EXPORT void CALL ProcessAList() { aur_fail("High-level audio lists are unsupported. Select the LLE RSP."); }
EXPORT void CALL SetSpeedFactor(int) {}
EXPORT void CALL VolumeUp() { level = std::min(100, level + 5); }
EXPORT void CALL VolumeDown() { level = std::max(0, level - 5); }
EXPORT int CALL VolumeGetLevel() { return level; }
EXPORT void CALL VolumeSetLevel(int value) { level = std::clamp(value, 0, 100); }
EXPORT void CALL VolumeMute() { aur_set_muted(!aur_get_muted()); }
EXPORT const char* CALL VolumeGetString() { return "Core Audio"; }
}
