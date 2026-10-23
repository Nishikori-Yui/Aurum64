# Validation record - 2026-10-23

Environment: Apple Silicon ARM64, macOS 27.2 (26B5091g), Xcode 27.1 (27A9275),
Swift 6.4, installed Metal compiler. Development bundle minimum: macOS 26.0.

## Completed locally

| Check | Actual result |
| --- | --- |
| Unmodified upstream core | Compiled as ARM64 with OSD=0 / NO_ASM=1 |
| Unmodified CXD4 RSP | Compiled scalar ARM64 plugin |
| Patched core and native app | Built via `scripts/build.sh` with Swift 6 |
| RDP/Metal unit executable | RGBA16/32 fills and VI conversion passed; seeded randomized reference comparisons included |
| Presentation executable | Offscreen RGBA-to-BGRA color order, vertical orientation and both filtering modes passed |
| Generated test ROM | 141 original MIPS instructions, 6 RDP commands, 8 VI events, 320x240 output, 1024 silent audio frames |
| RSP execution | Four original scalar instructions store a marker and BREAK; CPU observes marker and modifies two output pixels |
| Full framebuffer assertion | First two pixels green from RSP marker; remaining 76798 pixels exact orange from Metal fill |
| Lifecycle | Missing/bad ROM recovery, byte-swapped ROMs, repeated sessions, mailbox input, pause/resume/reset and stop passed |
| No-VI stop | Original infinite CPU loop is stopped by the interpreter hook without a VI event |
| Relocated bundle | Smoke/lifecycle tests used a temporary `.app` outside the repository |
| Actual dynamic loads | No Homebrew or build-tree dylib loaded during relocated smoke test; SDL3 dlopen dependency included |
| Signing | Ad-hoc `codesign --verify --deep --strict` passed |
| Metal validation layer | Entire test script passed with `MTL_DEBUG_LAYER=1`; no Metal API validation errors |
| Trace/replay | Captured 6 commands, replayed with 0 unsupported operations |
| App launch | LaunchServices started the built `.app` process |

Run `python3 scripts/test.py` after a build. Local test output is in
`.build/test.log` when redirected; the dyld audit is in `.build/tests/dyld.log`.

## Not validated / not complete

- **Visual desktop interaction:** Mac was locked when the UI inspector ran. No
  screenshots, keyboard focus, fullscreen, VoiceOver or window-close claims.
- **Commercial games:** no games were loaded or obtained; compatibility unknown.
- **Hardware accuracy:** comparisons currently use our scalar fill reference, not
  Angrylion/paraLLEl or captured N64 hardware. Fractional bounds, unsupported RDP
  modes and VI edge cases remain unverified.
- **Audio quality:** silent DMA exercised queue creation and delivery, not audible
  channel order, pitch, latency, underrun behavior or long-running synchronization.
- **Physical controllers:** mapping code compiled; no real controller exercise.
- **Saved games:** core cartridge persistence is wired but no game save/reload was
  tested. Save-state controls remain disabled to avoid incomplete restoration.
- **Performance:** no Instruments/GPU profiling or real-time claims; full RDRAM
  copies and synchronous GPU waits remain intentionally unoptimized.
- **CI:** workflow exists but has not run on GitHub. Hosted-runner GPU availability
  is an external requirement; absent Metal is a failure, not a false test pass.
- **Distribution:** no Developer ID signing, notarization, public release, or
  source-pinned Homebrew dependency rebuild. Packaging alone is not release readiness.

## Known upstream diagnostics

- Makefiles warn that ARM64 is not officially supported despite successful builds.
- Core warns about an int/4 GiB comparison and intentional-looking XOR expressions.
  These are upstream warnings; they were not silently modified.
- Generated ROM has no Nintendo CIC/IPL3; core logs an unknown CIC and selects 6102.
- Input adapter does not expose VRU support; upstream logs that limitation.
- Fresh isolated config directories log default-setting messages.
- `install_name_tool` invalidates copied signatures; the packager then re-signs all
  libraries and the app, followed by recursive verification.
