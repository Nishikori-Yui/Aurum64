# Aurum64

A macOS-only Nintendo 64 emulator project for Apple Silicon. The current
milestone integrates Mupen64Plus, a low-level CXD4 RSP, a native SwiftUI/AppKit
shell, Core Audio, GameController and an independent Metal compute renderer.

**This is an early emulator implementation, not a game-compatible release.**
The renderer implements fill-cycle RGBA16/RGBA32 rectangles, restricted
RGBA16/CI4/CI8 copy-cycle texture rectangles with RGBA16 TLUT, IA16 copy
rectangles, and a narrower IA8/I8 copy envelope with zero S/T origin, unit
derivatives, no flip and default tile addressing. Unsupported IA8/I8 address
variants are rejected. The renderer also has basic VI scanout. TMEM uploads and
signed point/copy coordinates have independent reference tests. One strictly
gated flat primitive-color RGBA16 triangle mode is wired through Metal; general
triangle attributes, other combiner modes, filtered textures, blender, depth,
coverage and accurate VI filters remain unfinished. The CPU uses the cached
interpreter, not a JIT.
There is no claim of real-time performance or commercial-game compatibility.

## Build and run

Requirements: Apple Silicon, macOS 26+, Xcode with Swift 6 and the Metal
toolchain, Homebrew build dependencies. The initial local build uses macOS 26
as its deployment target because the installed Homebrew libraries require it.
Older macOS deployment requires rebuilding those dependencies, not editing the
app's Info.plist. No iOS, Intel Mac or non-macOS targets are provided.

```sh
brew install cmake ninja pkg-config sdl2 libpng
# If Metal is not installed with Xcode:
xcodebuild -downloadComponent MetalToolchain

scripts/bootstrap.sh
scripts/build.sh
python3 scripts/test.py
open dist/Aurum64.app
```

The final `.app` embeds the core, plugins, Metal library, SDL dependencies,
libpng and license notices. Users do not need Homebrew or a separately installed
emulator core to run it. It is ad-hoc signed for development, not notarized.

Open an authorized, uncompressed `.z64`, `.v64` or `.n64` file. The native menu
offers pause/resume, stop, reset and standard macOS window/fullscreen controls.
Settings include display filtering, audio mute, input mapping help and a HUD.
Presentation smoothing is **not** higher-resolution emulation.

Configuration and core cartridge saves use
`~/Library/Application Support/Aurum64`. Keyboard/controller input currently
supports one player. Save states are deliberately disabled until renderer-state
serialization is integrated; cartridge SRAM/EEPROM/mempak support comes from the
core but game-driven persistence still needs validation.

## What the tests actually prove

- C++ RDP decoder: partial commands, triangle/rectangle command lengths,
  unsupported-command counters, fill/scissor bounds and invalid-input handling.
- Metal: RGBA16/RGBA32 fills, 48 seeded randomized comparisons with our scalar
  byte-address reference, 391 original RGBA16/CI4/CI8 copy draws compared with a
  separate pinned reference process, VI color conversion/blanking and offscreen presentation.
- Original generated ROM: CPU boot without proprietary IPL3, RDP DMA, Metal
  rendering, a small scalar RSP program, silent AI DMA and exact full-frame pixels.
  Texture variants also exercise production LoadTile, RGBA16 TLUT upload, CI8
  palette lookup, copy and alpha rejection.
- Lifecycle: invalid ROM recovery, all three ROM byte orders, 4 MiB RDRAM, repeated sessions,
  input mailbox, pause/resume/reset, stop while paused and stop without VI.
- Bundle relocation: smoke test outside the repository, actual dyld-load audit,
  recursive signature verification and RDP command capture/replay.

This does **not** establish hardware-accurate RDP output, real game compatibility,
vector RSP correctness, audible audio quality, physical controller operation,
or a completed UI accessibility/fullscreen review. See [validation](docs/VALIDATION.md).

## Repository layout

| Path | Responsibility |
| --- | --- |
| `Sources/AurumApp` | Swift 6 native UI, MetalKit display, GameController/keyboard |
| `Sources/AurumRuntime` | C ABI, emulation worker, command mailbox, frame snapshots |
| `Sources/Plugins` | Mupen video/audio/input adapters |
| `Sources/AurumRDP` | Independent MIT RDP decoder, Metal compute and presentation |
| `third_party` | Pinned upstream submodules |
| `patches` | Reproducible upstream extensions applied only to generated copies |
| `Tests` | Host, GPU, generated-ROM and lifecycle checks |
| `scripts` | Dependency initialization, build, packaging and validation |

Build outputs and all ROM extensions are ignored. Do not add game ROMs, BIOS,
Nintendo IPL3 code, textures, screenshots from copyrighted games or keys.

## Debugging

```sh
# Build a ROM entirely from our own source:
python3 scripts/make_test_rom.py .build/tests/fill.z64
python3 scripts/make_test_rom.py .build/tests/texture.z64 --texture

# Capture network-order RDP words (no implicit capture by default):
AURUM_RDP_TRACE="$PWD/.build/test.rdp" \
  .build/native/aurum-smoke .build/tests/fill.z64 \
  dist/Aurum64.app/Contents/Frameworks dist/Aurum64.app/Contents/Resources \
  .build/test-user

.build/native/aurum-rdp-tests .build/native/AurumRDP.metallib .build/test.rdp
.build/native/aurum-angrylion-reference .build/tests/fill.aurcap .build/tests/reference/fill
.build/native/aurum-reference-diff .build/tests/reference/fill .build/tests/reference/actual/fill .build/tests/fill.aurcap
```

Set `AURUM_VERBOSE=1` for upstream diagnostics. Traces currently contain commands
only, not RDRAM/TMEM/VI snapshots, so they replay self-contained fill streams,
not arbitrary games. Tests have external timeouts; a malformed non-yielding RSP
program can still hang its upstream interpreter.

The last command is a development-only, offline Angrylion-Plus reference pinned
in `third_party`. It reads the same `.aurcap` without ROM files, networking, or
the application runtime, prints machine-readable statistics, and optionally
writes raw RDRAM, hidden coverage bytes, TMEM, and per-frame VI color/depth/
coverage layers with a manifest. The diff tool reports the first differing
checkpoint or byte/pixel coordinate with expected/actual values. Neither tool is
part of `Aurum64.app`.

`aurum-aurcap-replay prefix` can emit the same neutral checkpoint/RDRAM/VI-color
format for Aurum's scalar supported-fill path. Its generated fill fixture
matches Angrylion bit-for-bit in RDRAM and pixel-for-pixel in direct VI color.
It emits verified TMEM uploads but lacks hidden state, depth, coverage, and filtered-VI output;
those remain explicit differential gaps rather than silently passing comparisons.

`aurum-metal-replay capture.aurcap output-prefix library.metallib` uses the
production Metal fill and texture-copy renderer while retaining the diagnostic
scalar VI-color export. Texture copy currently supports RGBA16 and CI4/CI8 with
RGBA16 TLUT plus IA16 and a constrained IA8/I8 copy subset, all with integer
rectangle/scissor edges and no perspective or LOD. IA8/I8 reject nonzero S/T
origins, non-unit derivatives, flip, nonzero tile origins and tile
shift/mask/mirror/clamp modes;
their broader qword lane/bank behavior is not yet modeled. Alpha comparison is
limited to the validated RGBA16 path. Unsupported modes, wrapped addresses and
unmodeled bank conflicts are rejected before any framebuffer write. The CPU
plan carries validated fetch selectors; Metal fetches TMEM, performs optional
palette/channel conversion and writes the pixels. This is an initial
synchronous implementation, with no performance claim.

## Next milestones

1. Versioned captures with memory/VI snapshots and an independently licensed
   software-reference harness; characterize RDP/VI edge cases before extending.
2. TMEM uploads and sampling, triangle setup/rasterization, combiner/blender,
   depth/coverage and VI reconstruction, with differential tests at each step.
3. Persist renderer state with core saves; validate cartridge persistence,
   controller hotplug, audio timing, accessibility and native app lifecycle.
4. ARM64 dynarec with macOS JIT constraints, tile scheduling, resource ownership
   and asynchronous frame delivery after the accuracy baseline is established.
5. Source-pinned build dependencies, Developer ID signing/notarization, source
   release archive and compatibility/performance evidence before a public release.

## Licensing

Application and core adapters: GPL-2.0-or-later. Independent renderer and original
ROM generator: MIT. Upstream code retains its own licenses. See
[dependency provenance](docs/DEPENDENCIES.md) and [architecture](docs/ARCHITECTURE.md).
