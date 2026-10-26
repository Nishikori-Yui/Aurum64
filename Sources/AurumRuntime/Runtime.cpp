// SPDX-License-Identifier: GPL-2.0-or-later
#include "AurumRuntime.h"
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
std::mutex mutex;
std::condition_variable changed;
AurSnapshot snapshot{};
std::vector<uint32_t> frame;
std::deque<int> commands;
std::thread worker;
std::atomic<uint32_t> buttons{0};
std::atomic<bool> muted{false};
std::atomic<uint32_t> rdramSize{0};
std::string shaderPath;
ptr_CoreDoCommand coreCommand = nullptr;
void (*resetVideo)() = nullptr;
uint64_t limit = 0;
std::array<uint64_t, 64> opcodeCounts{};
std::array<uint64_t, 64> firstOpcodeSubmission{};
std::array<uint64_t, 64> firstOpcodeVI{};
uint64_t rdpSubmissions = 0, viEvents = 0;
uint64_t recentRdpCommands = 0, recentUnsupportedCommands = 0;
uint32_t firstUnsupportedValid = 0, firstUnsupportedOpcode = 0;
uint32_t firstUnsupportedWordCount = 0, firstUnsupportedCapturedWords = 0;
uint64_t firstUnsupportedCommand = 0, firstUnsupportedSubmission = 0, firstUnsupportedVI = 0;
std::array<uint32_t, 32> firstUnsupportedWords{};
bool failed = false;
std::atomic<bool> pendingCommands{false};
void serviceCommands();

bool acceptsCommand(int command, int state) {
    switch (command) {
    case AUR_STOP:
        return state == AUR_STARTING || state == AUR_RUNNING || state == AUR_PAUSED;
    case AUR_PAUSE:
        return state == AUR_RUNNING;
    case AUR_RESUME:
        return state == AUR_PAUSED;
    case AUR_SAVE:
    case AUR_LOAD:
    case AUR_RESET:
        return state == AUR_RUNNING || state == AUR_PAUSED;
    default:
        return false;
    }
}

template<class T> T symbol(void* library, const char* name) {
    auto result = reinterpret_cast<T>(dlsym(library, name));
    if (!result) throw std::runtime_error(std::string("Missing core/plugin API: ") + name);
    return result;
}
void check(m64p_error error, const char* operation) {
    if (error != M64ERR_SUCCESS) throw std::runtime_error(std::string(operation) + " failed (Mupen error " + std::to_string(error) + ").");
}
void debug(void*, int level, const char* message) {
    if (level <= M64MSG_WARNING || std::getenv("AURUM_VERBOSE")) fprintf(stderr, "[Mupen:%d] %s\n", level, message);
}
void stateChanged(void*, m64p_core_param param, int value) {
    std::lock_guard lock(mutex);
    if (param == M64CORE_EMU_STATE && value == M64EMU_RUNNING) snapshot.state = AUR_RUNNING;
    if (param == M64CORE_STATE_LOADCOMPLETE || param == M64CORE_STATE_SAVECOMPLETE) {
        snprintf(snapshot.message, sizeof(snapshot.message), "%s %s.",
                 param == M64CORE_STATE_LOADCOMPLETE ? "Load state" : "Save state", value ? "completed" : "failed");
    }
}
void run(std::string rom, std::string frameworks, std::string resources, std::string userData) {
    void* core = nullptr;
    struct Plugin { void* handle; m64p_plugin_type type; bool started = false, attached = false; };
    std::vector<Plugin> plugins;
    bool started = false, opened = false;
    try {
        std::filesystem::create_directories(userData + "/Saves");
        std::filesystem::create_directories(userData + "/States");
        core = dlopen((frameworks + "/libmupen64plus.dylib").c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!core) throw std::runtime_error(dlerror());
        auto startup = symbol<ptr_CoreStartup>(core, "CoreStartup");
        coreCommand = symbol<ptr_CoreDoCommand>(core, "CoreDoCommand");
        symbol<void (*)(void (*)())>(core, "AurumSetExecutionCallback")(serviceCommands);
        check(startup(0x020001, userData.c_str(), resources.c_str(), nullptr, debug, nullptr, stateChanged), "Core startup");
        started = true;
        auto openSection = symbol<ptr_ConfigOpenSection>(core, "ConfigOpenSection");
        auto set = symbol<ptr_ConfigSetParameter>(core, "ConfigSetParameter");
        m64p_handle config;
        check(openSection("Core", &config), "Core configuration");
        int one = 1, zero = 0;
        check(set(config, "R4300Emulator", M64TYPE_INT, &one), "Interpreter configuration");
        check(set(config, "OnScreenDisplay", M64TYPE_BOOL, &zero), "OSD configuration");
        check(set(config, "SaveSRAMPath", M64TYPE_STRING, (userData + "/Saves").c_str()), "Save directory");
        check(set(config, "SaveStatePath", M64TYPE_STRING, (userData + "/States").c_str()), "State directory");
        std::ifstream stream(rom, std::ios::binary | std::ios::ate);
        const auto length = stream.tellg();
        if (!stream || length < 4096 || length > 64 * 1024 * 1024 || length % 4 != 0)
            throw std::runtime_error("Choose an uncompressed N64 ROM between 4 KiB and 64 MiB, aligned to 4 bytes.");
        std::vector<char> bytes(static_cast<size_t>(length)); stream.seekg(0);
        if (!stream.read(bytes.data(), length)) throw std::runtime_error("Cannot read the ROM file.");
        check(coreCommand(M64CMD_ROM_OPEN, int(bytes.size()), bytes.data()), "ROM open"); opened = true;
        auto attach = symbol<ptr_CoreAttachPlugin>(core, "CoreAttachPlugin");
        const std::pair<const char*, m64p_plugin_type> names[] = {
            {"libaurum-video.dylib", M64PLUGIN_GFX}, {"libaurum-audio.dylib", M64PLUGIN_AUDIO},
            {"libaurum-input.dylib", M64PLUGIN_INPUT}, {"mupen64plus-rsp-cxd4.dylib", M64PLUGIN_RSP}};
        for (auto [name, type] : names) {
            void* handle = dlopen((frameworks + "/" + name).c_str(), RTLD_NOW | RTLD_LOCAL);
            if (!handle) throw std::runtime_error(dlerror());
            plugins.push_back({handle, type});
            check(symbol<ptr_PluginStartup>(handle, "PluginStartup")(core, nullptr, debug), "Plugin startup");
            plugins.back().started = true;
            // InitiateRSP reads these settings during attachment, not during EXECUTE.
            if (type == M64PLUGIN_RSP) {
                check(openSection("rsp-cxd4", &config), "RSP configuration");
                check(set(config, "DisplayListToGraphicsPlugin", M64TYPE_BOOL, &zero), "LLE graphics");
                check(set(config, "AudioListToAudioPlugin", M64TYPE_BOOL, &zero), "LLE audio");
            }
            check(attach(type, handle), "Plugin attachment"); plugins.back().attached = true;
            if (type == M64PLUGIN_GFX) resetVideo = symbol<void (*)()>(handle, "AurumResetVideo");
        }
        // Plugin attachment can report a fatal error (for example when the
        // renderer cannot create its Metal resources). Do not enter the core
        // with a partially initialized plugin set in that case.
        bool startupFailed;
        {
            std::lock_guard lock(mutex);
            startupFailed = failed;
        }
        if (!startupFailed) check(coreCommand(M64CMD_EXECUTE, 0, nullptr), "Emulation");
    } catch (const std::exception& error) { aur_fail(error.what()); }
    // All cleanup remains on the core-owning thread, including partial startup failures.
    resetVideo = nullptr;
    for (auto p = plugins.rbegin(); p != plugins.rend(); ++p) {
        if (p->attached) symbol<ptr_CoreDetachPlugin>(core, "CoreDetachPlugin")(p->type);
        if (p->started) symbol<ptr_PluginShutdown>(p->handle, "PluginShutdown")();
        dlclose(p->handle);
    }
    if (opened) coreCommand(M64CMD_ROM_CLOSE, 0, nullptr);
    if (started) {
        symbol<ptr_ConfigSaveFile>(core, "ConfigSaveFile")();
        symbol<ptr_CoreShutdown>(core, "CoreShutdown")();
    }
    coreCommand = nullptr;
    if (core) dlclose(core);
    { std::lock_guard lock(mutex); snapshot.state = failed ? AUR_FAILED : AUR_STOPPED; }
    changed.notify_all();
}
}
int aur_start(const char* rom, const char* frameworks, const char* resources, const char* data, uint64_t frameLimit) {
    if (!rom || !*rom || !frameworks || !*frameworks || !resources || !*resources || !data || !*data) return 0;
    {
        std::lock_guard lock(mutex);
        if (snapshot.state != AUR_STOPPED && snapshot.state != AUR_FAILED) return 0;
        // Reserve the runtime before joining a failed worker. This prevents a
        // concurrent caller from trying to join or replace the same thread.
        snapshot.state = AUR_STARTING;
    }
    // A failed session can publish AUR_FAILED before its worker has finished
    // unloading the core and plugins. Reap that worker before resetting the
    // shared state, otherwise a new session can race with old cleanup.
    if (worker.joinable()) worker.join();
    {
        std::lock_guard lock(mutex);
        snapshot = {}; snapshot.state = AUR_STARTING; frame.clear(); commands.clear();
        failed = false; pendingCommands = false; limit = frameLimit; buttons = 0; rdramSize = 0;
        opcodeCounts.fill(0);
        firstOpcodeSubmission.fill(UINT64_MAX);
        firstOpcodeVI.fill(UINT64_MAX);
        rdpSubmissions = viEvents = 0;
        recentRdpCommands = recentUnsupportedCommands = 0;
        firstUnsupportedValid = firstUnsupportedOpcode = 0;
        firstUnsupportedWordCount = firstUnsupportedCapturedWords = 0;
        firstUnsupportedCommand = firstUnsupportedSubmission = firstUnsupportedVI = 0;
        firstUnsupportedWords.fill(0);
        shaderPath = std::string(resources) + "/AurumRDP.metallib";
    }
    // Swift's temporary C strings expire when aur_start returns. Construct owned
    // strings here; std::thread would otherwise retain only the borrowed pointers.
    worker = std::thread(run, std::string(rom), std::string(frameworks),
                         std::string(resources), std::string(data));
    return 1;
}
void aur_command(int command) {
    std::lock_guard lock(mutex);
    if (!acceptsCommand(command, snapshot.state)) return;
    commands.push_back(command); pendingCommands = true; changed.notify_all();
}
void aur_snapshot(AurSnapshot* out) { if (out) { std::lock_guard lock(mutex); *out = snapshot; } }
int aur_diagnostics(AurDiagnostics* out, size_t size) {
    if (!out || size < sizeof(AurDiagnostics)) return 0;
    std::lock_guard lock(mutex);
    AurDiagnostics result{};
    result.size = sizeof(AurDiagnostics);
    result.version = AUR_DIAGNOSTICS_VERSION;
    result.state = snapshot.state;
    result.width = snapshot.width;
    result.height = snapshot.height;
    result.frames = snapshot.frames;
    result.rdpCommands = snapshot.rdpCommands;
    result.unsupportedCommands = snapshot.unsupportedCommands;
    result.audioFrames = snapshot.audioFrames;
    result.rdpSubmissions = rdpSubmissions;
    result.viEvents = viEvents;
    result.recentRdpCommands = recentRdpCommands;
    result.recentUnsupportedCommands = recentUnsupportedCommands;
    result.firstUnsupportedValid = firstUnsupportedValid;
    result.firstUnsupportedOpcode = firstUnsupportedOpcode;
    result.firstUnsupportedWordCount = firstUnsupportedWordCount;
    result.firstUnsupportedCapturedWords = firstUnsupportedCapturedWords;
    result.firstUnsupportedCommand = firstUnsupportedCommand;
    result.firstUnsupportedSubmission = firstUnsupportedSubmission;
    result.firstUnsupportedVI = firstUnsupportedVI;
    std::memcpy(result.firstUnsupportedWords, firstUnsupportedWords.data(),
                sizeof(result.firstUnsupportedWords));
    std::memcpy(result.opcodeCounts, opcodeCounts.data(), sizeof(result.opcodeCounts));
    std::memcpy(result.firstOpcodeSubmission, firstOpcodeSubmission.data(), sizeof(result.firstOpcodeSubmission));
    std::memcpy(result.firstOpcodeVI, firstOpcodeVI.data(), sizeof(result.firstOpcodeVI));
    std::memcpy(out, &result, sizeof(result));
    return 1;
}
size_t aur_copy_frame(uint32_t* output, size_t capacity, uint32_t* width, uint32_t* height) {
    std::lock_guard lock(mutex);
    if (width) *width = snapshot.width;
    if (height) *height = snapshot.height;
    if (output && !frame.empty() && capacity >= frame.size()) std::memcpy(output, frame.data(), frame.size() * 4);
    return frame.size();
}
void aur_set_buttons(uint32_t value) { buttons.store(value); }
uint32_t aur_get_buttons() { return buttons.load(); }
void aur_set_muted(int value) { muted.store(value != 0); }
int aur_get_muted() { return muted.load(); }
int aur_wait() {
    if (worker.joinable()) worker.join();
    std::lock_guard lock(mutex); return snapshot.state == AUR_FAILED ? 1 : 0;
}
void aur_publish_frame(const uint32_t* pixels, uint32_t width, uint32_t height, uint64_t count, uint64_t unsupported) {
    std::lock_guard lock(mutex);
    if (pixels && width && height) frame.assign(pixels, pixels + width * height); else frame.clear();
    snapshot.width = width; snapshot.height = height;
    snapshot.rdpCommands = count; snapshot.unsupportedCommands = unsupported;
}
void aur_publish_rdp_diagnostics(const uint64_t* counts, const uint64_t* submissions,
                                 const uint64_t* firstVI, uint64_t submissionCount, uint64_t viCount,
                                 uint64_t recentCommands, uint64_t recentUnsupported) {
    if (!counts || !submissions || !firstVI) return;
    std::lock_guard lock(mutex);
    std::memcpy(opcodeCounts.data(), counts, sizeof(opcodeCounts));
    std::memcpy(firstOpcodeSubmission.data(), submissions, sizeof(firstOpcodeSubmission));
    std::memcpy(firstOpcodeVI.data(), firstVI, sizeof(firstOpcodeVI));
    rdpSubmissions = submissionCount;
    viEvents = viCount;
    recentRdpCommands = recentCommands;
    recentUnsupportedCommands = recentUnsupported;
}
void aur_publish_unsupported_context(uint32_t valid, uint32_t opcode,
                                     uint32_t wordCount, uint32_t capturedWords,
                                     uint64_t command, uint64_t submission, uint64_t vi,
                                     const uint32_t* words) {
    std::lock_guard lock(mutex);
    if (firstUnsupportedValid || !valid || !words) return;
    firstUnsupportedValid = 1;
    firstUnsupportedOpcode = opcode;
    firstUnsupportedWordCount = wordCount;
    firstUnsupportedCapturedWords = std::min<uint32_t>(
        capturedWords, static_cast<uint32_t>(firstUnsupportedWords.size()));
    firstUnsupportedCommand = command;
    firstUnsupportedSubmission = submission;
    firstUnsupportedVI = vi;
    std::memcpy(firstUnsupportedWords.data(), words,
                firstUnsupportedCapturedWords * sizeof(uint32_t));
}
void aur_video_tick() {
    {
        std::lock_guard lock(mutex);
        ++snapshot.frames;
        if (limit && snapshot.frames >= limit) { commands.push_back(AUR_STOP); pendingCommands = true; }
    }
    // Idle-loop acceleration can advance an entire VI in one interpreted instruction.
    serviceCommands();
}
namespace {
void serviceCommands() {
    if (!pendingCommands.load(std::memory_order_relaxed)) return;
    std::unique_lock lock(mutex);
    if (failed) commands.push_back(AUR_STOP);
    do {
        while (!commands.empty()) {
            int command = commands.front(); commands.pop_front();
            if (command == AUR_PAUSE) { snapshot.state = AUR_PAUSED; continue; }
            if (command == AUR_RESUME) { snapshot.state = AUR_RUNNING; continue; }
            if (command == AUR_STOP) snapshot.state = AUR_STOPPING;
            lock.unlock();
            switch (command) {
            case AUR_STOP: coreCommand(M64CMD_STOP, 0, nullptr); break;
            case AUR_SAVE: case AUR_LOAD: {
                std::lock_guard guard(mutex);
                snprintf(snapshot.message, sizeof(snapshot.message), "Save states need renderer-state serialization and are not available yet.");
                break;
            }
            case AUR_RESET:
                if (resetVideo) resetVideo();
                coreCommand(M64CMD_RESET, 1, nullptr); break;
            }
            lock.lock();
            if (command == AUR_STOP) { commands.clear(); pendingCommands = false; return; }
        }
        if (snapshot.state == AUR_PAUSED) {
            changed.wait(lock, [] { return !commands.empty() || failed; });
            if (failed) commands.push_back(AUR_STOP);
        }
    } while (snapshot.state == AUR_PAUSED || !commands.empty());
    pendingCommands = false;
}
}
void aur_audio_frames(uint32_t count) { std::lock_guard lock(mutex); snapshot.audioFrames += count; }
void aur_set_rdram_size(uint32_t bytes) { rdramSize = bytes; }
uint32_t aur_get_rdram_size() { return rdramSize.load(); }
void aur_fail(const char* message) {
    std::lock_guard lock(mutex); failed = true; pendingCommands = true;
    changed.notify_all();
    snprintf(snapshot.message, sizeof(snapshot.message), "%s", message);
}
const char* aur_shader_path() { return shaderPath.c_str(); }
