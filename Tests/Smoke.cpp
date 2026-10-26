// SPDX-License-Identifier: GPL-2.0-or-later
#include "AurumRuntime.h"
#include <cstdio>
#include <vector>
#include <cstring>
#include <dlfcn.h>
#include <string>
int main(int argc, char** argv) {
    if (argc != 5 && !(argc == 6 && std::strcmp(argv[5], "--texture") == 0)) {
        std::fprintf(stderr,"Usage: aurum-smoke ROM Frameworks Resources UserData [--texture]\n"); return 2;
    }
    const bool textured = argc == 6;
    void* runtime = dlopen((std::string(argv[2]) + "/libaurum-runtime.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!runtime) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
    auto aur_start = reinterpret_cast<decltype(&::aur_start)>(dlsym(runtime, "aur_start"));
    auto aur_wait = reinterpret_cast<decltype(&::aur_wait)>(dlsym(runtime, "aur_wait"));
    auto aur_snapshot = reinterpret_cast<decltype(&::aur_snapshot)>(dlsym(runtime, "aur_snapshot"));
    auto aur_diagnostics = reinterpret_cast<decltype(&::aur_diagnostics)>(dlsym(runtime, "aur_diagnostics"));
    auto aur_copy_frame = reinterpret_cast<decltype(&::aur_copy_frame)>(dlsym(runtime, "aur_copy_frame"));
    if (!aur_start || !aur_wait || !aur_snapshot || !aur_diagnostics || !aur_copy_frame) return 1;
    if (!aur_start(argv[1], argv[2], argv[3], argv[4], 8)) return 1;
    const int result = aur_wait(); AurSnapshot state; aur_snapshot(&state);
    AurDiagnostics diagnostics{};
    if (!aur_diagnostics(&diagnostics, sizeof(diagnostics)) ||
        diagnostics.size != sizeof(AurDiagnostics) ||
        diagnostics.version != AUR_DIAGNOSTICS_VERSION ||
        diagnostics.state != state.state ||
        diagnostics.frames != state.frames ||
        diagnostics.rdpCommands != state.rdpCommands ||
        diagnostics.unsupportedCommands != state.unsupportedCommands ||
        diagnostics.audioFrames != state.audioFrames ||
        diagnostics.rdpSubmissions != 1 ||
        diagnostics.viEvents != 8 ||
        diagnostics.recentRdpCommands > diagnostics.rdpCommands ||
        diagnostics.recentUnsupportedCommands > diagnostics.unsupportedCommands ||
        diagnostics.opcodeCounts[0x3f] != 1 ||
        diagnostics.firstOpcodeSubmission[0x3f] != 1 ||
        diagnostics.firstOpcodeVI[0x3f] != 1 ||
        (textured && (diagnostics.opcodeCounts[0x34] != 1 ||
                      diagnostics.opcodeCounts[0x24] != 1)) ||
        diagnostics.firstUnsupportedValid != 0 ||
        aur_diagnostics(&diagnostics, sizeof(diagnostics) - 1)) {
        std::fprintf(stderr, "Versioned diagnostics ABI mismatch\n");
        return 1;
    }
    std::printf("state=%d VI=%llu size=%ux%u RDP=%llu unsupported=%llu audioFrames=%llu error=%s\n",
        state.state,state.frames,state.width,state.height,state.rdpCommands,state.unsupportedCommands,state.audioFrames,state.message);
    if (result || state.frames < 8 || state.width != 320 || state.height != 240 ||
        state.rdpCommands != (textured ? 11 : 6) || state.unsupportedCommands || !state.audioFrames)
        return 1;
    std::vector<uint32_t> pixels(320*240); uint32_t w,h;
    aur_copy_frame(pixels.data(), pixels.size(), &w, &h);
    for (size_t i = 0; i < pixels.size(); ++i) {
        uint32_t expected = i < 2 ? 0xff00f800u : 0xff2860e8u;
        const uint32_t x = i % 320, y = i / 320;
        if (textured && x >= 8 && x <= 311 && y >= 8 && y <= 231 && ((x + y) & 1))
            expected = (y & 7) < 4 ?
                ((x & 7) < 4 ? 0xff0000f8 : 0xff00f800) :
                ((x & 7) < 4 ? 0xfff80000 : 0xfff8f8f8);
        if (pixels[i] != expected) {
            std::fprintf(stderr,"Pixel mismatch at %zu: %08x expected %08x\n",i,pixels[i],expected); return 1;
        }
    }
    std::puts(textured ? "PASS: original textured MIPS ROM -> CPU/RSP/AI -> Metal TMEM copy -> VI -> host frame" :
                        "PASS: generated MIPS ROM -> Mupen CPU + CXD4 RSP + AI DMA -> Metal fill -> VI -> host frame");
}
