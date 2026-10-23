// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common.hpp"
#include "Renderer.hpp"
#include <cstring>
#include <fstream>
#include <cstdlib>

AUR_PLUGIN(M64PLUGIN_GFX, 0x020200, "Aurum Metal RDP (experimental fill/VI)")
namespace {
GFX_INFO info{};
aurum::rdp::Decoder decoder;
std::unique_ptr<aurum::rdp::Renderer> renderer;
std::vector<uint32_t> pixels;
uint32_t width = 0, height = 0;
void (*renderCallback)(int) = nullptr;
std::ofstream trace;
void publishDiagnostics() {
    uint64_t counts[64], submissions[64], vis[64];
    for (unsigned opcode = 0; opcode < 64; ++opcode) {
        counts[opcode] = decoder.opcodeCount(opcode);
        submissions[opcode] = decoder.firstSubmission(opcode);
        vis[opcode] = decoder.firstVI(opcode);
    }
    aur_publish_rdp_diagnostics(counts, submissions, vis,
                                decoder.submissionCount(), decoder.viCount(),
                                decoder.recentCommands(), decoder.recentUnsupported());
}
size_t memorySize() { return info.RDRAM_SIZE ? *info.RDRAM_SIZE : 8 * 1024 * 1024; }
}
extern "C" {
EXPORT int CALL InitiateGFX(GFX_INFO value) { info = value; return info.version >= 2; }
EXPORT int CALL RomOpen() {
    aur_set_rdram_size(uint32_t(memorySize()));
    renderer = std::make_unique<aurum::rdp::Renderer>();
    std::string error;
    if (!renderer->initialize(aur_shader_path(), error)) { aur_fail(error.c_str()); return 0; }
    decoder.reset(); width = height = 0; pixels.clear();
    publishDiagnostics();
    decoder.fill = [](const aurum::rdp::Fill& fill) {
        std::string error;
        if (!renderer->fill(info.RDRAM, memorySize(), fill, error)) aur_fail(error.c_str());
    };
    decoder.sync = [] { *info.MI_INTR_REG |= 0x20; info.CheckInterrupts(); };
    if (const char* path = std::getenv("AURUM_RDP_TRACE")) trace.open(path, std::ios::binary | std::ios::trunc);
    return 1;
}
EXPORT void CALL RomClosed() { if (trace.is_open()) trace.close(); trace.clear(); renderer.reset(); }
EXPORT void CALL AurumResetVideo() { decoder.reset(); pixels.clear(); width = height = 0; }
EXPORT void CALL ProcessRDPList() {
    uint32_t current = *info.DPC_CURRENT_REG & 0x00fffff8;
    const uint32_t end = *info.DPC_END_REG & 0x00fffff8;
    const bool xbus = *info.DPC_STATUS_REG & 1;
    if (end < current || end - current > memorySize()) { aur_fail("Invalid RDP command range."); return; }
    decoder.beginSubmission();
    for (; current < end; current += 8) {
        uint32_t words[2];
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t address = current + i * 4;
            if (xbus) std::memcpy(&words[i], info.DMEM + (address & 4095), 4);
            else {
                if (address + 4 > memorySize()) { aur_fail("RDP command exceeds RDRAM."); return; }
                std::memcpy(&words[i], info.RDRAM + address, 4);
            }
            // Trace format is network-order words, independent of the host's endianness.
            if (trace) { uint32_t be = __builtin_bswap32(words[i]); trace.write(reinterpret_cast<char*>(&be), 4); }
        }
        decoder.push(words);
    }
    *info.DPC_CURRENT_REG = end;
    publishDiagnostics();
}
EXPORT void CALL UpdateScreen() {
    if (renderer) {
        decoder.beginVI();
        std::string error;
        aurum::rdp::VideoInterface vi{*info.VI_STATUS_REG, *info.VI_ORIGIN_REG, *info.VI_WIDTH_REG,
            *info.VI_H_START_REG, *info.VI_V_START_REG, *info.VI_X_SCALE_REG, *info.VI_Y_SCALE_REG};
        if (!renderer->scanout(info.RDRAM, memorySize(), vi, pixels, width, height, error)) aur_fail(error.c_str());
        aur_publish_frame(pixels.data(), width, height, decoder.commands, decoder.unsupported);
        publishDiagnostics();
        if (renderCallback) renderCallback(1);
    }
    aur_video_tick();
}
EXPORT void CALL ProcessDList() { aur_fail("High-level display lists are unsupported. Select the LLE RSP."); }
EXPORT void CALL ShowCFB() {}
EXPORT void CALL ChangeWindow() {}
EXPORT void CALL MoveScreen(int, int) {}
EXPORT void CALL ViStatusChanged() {}
EXPORT void CALL ViWidthChanged() {}
EXPORT void CALL ResizeVideoOutput(int, int) {}
EXPORT void CALL SetRenderingCallback(void (*callback)(int)) { renderCallback = callback; }
EXPORT void CALL FBRead(unsigned int) {}
EXPORT void CALL FBWrite(unsigned int, unsigned int) {}
EXPORT void CALL FBGetFrameBufferInfo(void* p) { std::memset(p, 0, sizeof(FrameBufferInfo) * 6); }
EXPORT void CALL ReadScreen2(void* destination, int* w, int* h, int) {
    *w = int(width); *h = int(height);
    if (!destination) return;
    auto* bytes = static_cast<uint8_t*>(destination);
    for (size_t i = 0; i < pixels.size(); ++i) {
        bytes[3*i] = pixels[i] & 255; bytes[3*i+1] = (pixels[i] >> 8) & 255; bytes[3*i+2] = (pixels[i] >> 16) & 255;
    }
}
}
