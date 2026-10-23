// SPDX-License-Identifier: GPL-2.0-or-later
#include "AurumRuntime.h"
#include <cstdio>
#include <vector>
#include <cstring>
#include <dlfcn.h>
#include <string>
int main(int argc, char** argv) {
    if (argc != 5) { std::fprintf(stderr,"Usage: aurum-smoke ROM Frameworks Resources UserData\n"); return 2; }
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
        diagnostics.firstUnsupportedValid != 0 ||
        aur_diagnostics(&diagnostics, sizeof(diagnostics) - 1)) {
        std::fprintf(stderr, "Versioned diagnostics ABI mismatch\n");
        return 1;
    }
    std::printf("state=%d VI=%llu size=%ux%u RDP=%llu unsupported=%llu audioFrames=%llu error=%s\n",
        state.state,state.frames,state.width,state.height,state.rdpCommands,state.unsupportedCommands,state.audioFrames,state.message);
    if (result || state.frames < 8 || state.width != 320 || state.height != 240 || state.rdpCommands != 6 || state.unsupportedCommands || !state.audioFrames)
        return 1;
    std::vector<uint32_t> pixels(320*240); uint32_t w,h;
    aur_copy_frame(pixels.data(), pixels.size(), &w, &h);
    for (size_t i = 0; i < pixels.size(); ++i) if (pixels[i] != (i < 2 ? 0xff00ff00u : 0xff2963efu)) {
        std::fprintf(stderr,"Pixel mismatch at %zu: %08x (RSP marker or RDP fill)\n",i,pixels[i]); return 1;
    }
    std::puts("PASS: generated MIPS ROM -> Mupen CPU + CXD4 RSP + AI DMA -> Metal fill -> VI -> host frame");
}
