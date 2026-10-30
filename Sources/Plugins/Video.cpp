// SPDX-License-Identifier: GPL-2.0-or-later
#include "Common.hpp"
#include "Capture.hpp"
#include "DpcDispatch.hpp"
#include "Renderer.hpp"
#include "TriangleMode.hpp"
#include "TextureUpload.hpp"
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <utility>

AUR_PLUGIN(M64PLUGIN_GFX, 0x020200, "Aurum Metal RDP (experimental fill/copy/VI)")
namespace {
GFX_INFO info{};
aurum::rdp::Decoder decoder;
aurum::texture::Uploader textures;
std::unique_ptr<aurum::rdp::Renderer> renderer;
std::vector<uint32_t> pixels;
uint32_t width = 0, height = 0;
void (*renderCallback)(int) = nullptr;
std::ofstream trace;
std::string capturePath;
std::vector<aurum::capture::Record> captureRecords;
uint64_t captureSequence = 0;
uint64_t captureBytes = 0, captureMaxBytes = 0, captureMaxVI = 0, captureVI = 0;
bool captureStopped = false;
constexpr uint64_t kCaptureHeaderBytes = 32;
constexpr uint64_t kCaptureChunkHeaderBytes = 24;
constexpr uint64_t kCaptureEndReserve = kCaptureChunkHeaderBytes + 8 + sizeof("byte limit") - 1;
void appendLE(std::vector<uint8_t>& payload, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) payload.push_back(uint8_t(value >> (i * 8)));
}
void appendBE(std::vector<uint8_t>& payload, uint32_t value) {
    for (int i = 3; i >= 0; --i) payload.push_back(uint8_t(value >> (i * 8)));
}
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
    const auto& unsupported = decoder.firstUnsupported();
    aur_publish_unsupported_context(unsupported.valid, unsupported.opcode,
                                    unsupported.wordCount, unsupported.capturedWords,
                                    unsupported.command, unsupported.submission,
                                    unsupported.vi, unsupported.words);
}
void writeCapture() {
    if (capturePath.empty()) return;
    std::ofstream output(capturePath, std::ios::binary | std::ios::trunc);
    std::string error;
    if (!output || !aurum::capture::write(output, captureRecords, error)) aur_fail(error.c_str());
}
void finishCapture(const char* reason) {
    if (capturePath.empty() || captureStopped) return;
    std::vector<uint8_t> payload;
    appendLE(payload, uint32_t(decoder.commands));
    appendLE(payload, uint32_t(decoder.unsupported));
    while (*reason) payload.push_back(uint8_t(*reason++));
    captureRecords.push_back({aurum::capture::kRecordEnd, captureSequence++, std::move(payload)});
    writeCapture();
    capturePath.clear();
    captureRecords.clear();
    captureStopped = true;
}
bool captureAllows(uint64_t additionalBytes) {
    if (capturePath.empty() || captureStopped) return false;
    const uint64_t limit = captureMaxBytes
        ? std::min(captureMaxBytes, aurum::capture::kMaxCaptureBytes)
        : aurum::capture::kMaxCaptureBytes;
    if (captureBytes > limit || additionalBytes > limit - captureBytes ||
        kCaptureEndReserve > limit - captureBytes - additionalBytes) {
        finishCapture("byte limit");
        return false;
    }
    return true;
}
void appendCapture(aurum::capture::Record record) {
    if (!captureAllows(record.payload.size() + kCaptureChunkHeaderBytes)) return;
    captureBytes += record.payload.size() + kCaptureChunkHeaderBytes;
    captureRecords.push_back(std::move(record));
}
uint64_t environmentLimit(const char* name) {
    const char* text = std::getenv(name);
    if (!text || !*text) return 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    return end && !*end ? uint64_t(value) : 0;
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
    decoder.reset(); textures = {}; width = height = 0; pixels.clear();
    publishDiagnostics();
    capturePath = std::getenv("AURUM_AURCAP") ? std::getenv("AURUM_AURCAP") : "";
    captureRecords.clear(); captureSequence = 0;
    if (!capturePath.empty()) {
        const uint32_t size = uint32_t(memorySize());
        std::vector<uint8_t> payload;
        appendLE(payload, size);
        appendLE(payload, 0);
        captureMaxBytes = environmentLimit("AURUM_AURCAP_MAX_BYTES");
        captureMaxVI = environmentLimit("AURUM_AURCAP_MAX_VI");
        captureVI = 0; captureStopped = false;
        captureBytes = kCaptureHeaderBytes;
        constexpr uint64_t minimumCaptureBytes = kCaptureHeaderBytes +
            kCaptureChunkHeaderBytes + 8 + kCaptureEndReserve;
        if (captureMaxBytes && captureMaxBytes < minimumCaptureBytes) {
            capturePath.clear();
            aur_fail("AURUM_AURCAP_MAX_BYTES is too small for a valid capture.");
        }
        appendCapture({aurum::capture::kRecordInitial, captureSequence++, std::move(payload)});
    }
    decoder.fill = [](const aurum::rdp::Fill& fill) {
        std::string error;
        if (!renderer->fill(info.RDRAM, memorySize(), fill, error)) aur_fail(error.c_str());
    };
    decoder.textureLoad = [](const aurum::rdp::TextureLoad& load) {
        return textures.load(info.RDRAM, memorySize(), load);
    };
    decoder.textureRect = [](const aurum::rdp::TextureRect& rect) {
        std::string error;
        return renderer->textureCopy(info.RDRAM, memorySize(), rect, textures.store(), error);
    };
    decoder.triangle = [](const aurum::rdp::Triangle& triangle) {
        std::string error;
        if (aurum::rdp::isDepthTriangle(triangle))
            return renderer->depthTriangle(info.RDRAM, memorySize(), triangle, error);
        if (aurum::rdp::isSimpleCombinerTriangle(triangle))
            return renderer->combinerTriangle(info.RDRAM, memorySize(), triangle, textures.store(), error);
        if (aurum::rdp::isTexturedTriangle(triangle))
            return renderer->texturedTriangle(info.RDRAM, memorySize(), triangle, textures.store(), error);
        if (!aurum::rdp::isColorTriangle(triangle)) return false;
        return renderer->flatTriangle(
            info.RDRAM, memorySize(), triangle.fbAddress, triangle.fbWidth,
            triangle.fbSize, aurum::rdp::primitiveColor16(triangle.primitiveColor), triangle,
            int32_t(triangle.clipXh), int32_t(triangle.clipYh),
            int32_t(triangle.clipXl), int32_t(triangle.clipYl), error);
    };
    decoder.sync = [] { *info.MI_INTR_REG |= 0x20; info.CheckInterrupts(); };
    if (const char* path = std::getenv("AURUM_RDP_TRACE")) trace.open(path, std::ios::binary | std::ios::trunc);
    return 1;
}
EXPORT void CALL RomClosed() {
    if (!capturePath.empty()) finishCapture("rom closed");
    if (trace.is_open()) trace.close();
    trace.clear(); capturePath.clear(); captureRecords.clear(); renderer.reset();
}
EXPORT void CALL AurumResetVideo() {
    decoder.reset(); textures = {}; pixels.clear(); width = height = 0;
    if (renderer) renderer->resetHidden();
}
EXPORT void CALL ProcessRDPList() {
    uint32_t current = *info.DPC_CURRENT_REG & 0x00fffff8;
    const uint32_t start = current;
    const uint32_t end = *info.DPC_END_REG & 0x00fffff8;
    const uint32_t status = *info.DPC_STATUS_REG;
    const bool xbus = status & aurum::dpc::kXbusDmemDma;
    if (!aurum::dpc::shouldProcess(status)) return;
    // The decoder executes synchronously, so DPC flush has no deferred queue to drain.
    if (aurum::dpc::flushRequested(status)) publishDiagnostics();
    if (!aurum::dpc::validRange(current, end, xbus, memorySize())) {
        aur_fail("Invalid RDP command range."); return;
    }
    decoder.beginSubmission();
    while (current < end) {
        uint32_t words[2];
        const uint32_t count = aurum::dpc::wordsAvailable(current, end);
        for (uint32_t index = 0; index < count; ++index, current += 4) {
            const uint32_t address = current;
            if (xbus) std::memcpy(&words[index], info.DMEM + aurum::dpc::busAddress(address, true), 4);
            else {
                if (address + 4 > memorySize()) { aur_fail("RDP command exceeds RDRAM."); return; }
                std::memcpy(&words[index], info.RDRAM + aurum::dpc::busAddress(address, false), 4);
            }
            // Trace format is network-order words, independent of the host's endianness.
            if (trace) { uint32_t be = __builtin_bswap32(words[index]); trace.write(reinterpret_cast<char*>(&be), 4); }
        }
        decoder.push(std::span(words, count));
    }
    if (!capturePath.empty()) {
        std::vector<uint8_t> payload;
        appendLE(payload, start);
        appendLE(payload, end);
        appendLE(payload, status);
        appendLE(payload, xbus ? 1u : 0u);
        for (uint32_t address = start; address < end; address += 4) {
            uint32_t word;
            if (xbus) std::memcpy(&word, info.DMEM + aurum::dpc::busAddress(address, true), 4);
            else std::memcpy(&word, info.RDRAM + aurum::dpc::busAddress(address, false), 4);
            appendBE(payload, word);
        }
        appendCapture({aurum::capture::kRecordRDP, captureSequence++, std::move(payload)});
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
        if (!capturePath.empty()) {
            std::vector<uint8_t> payload;
            appendLE(payload, *info.VI_STATUS_REG);
            appendLE(payload, *info.VI_ORIGIN_REG);
            appendLE(payload, *info.VI_WIDTH_REG);
            appendLE(payload, *info.VI_H_START_REG);
            appendLE(payload, *info.VI_V_START_REG);
            appendLE(payload, *info.VI_X_SCALE_REG);
            appendLE(payload, *info.VI_Y_SCALE_REG);
            if (captureAllows(24 + payload.size())) {
                ++captureVI;
                appendCapture({aurum::capture::kRecordVI, captureSequence++, std::move(payload)});
                if (captureMaxVI && captureVI >= captureMaxVI) finishCapture("VI limit");
            }
        }
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
