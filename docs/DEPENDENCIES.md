# Dependencies and source provenance

Checked on 2026-10-23. Build inputs are submodule commits, not floating branches.

| Component | Source / pin | License | Use |
| --- | --- | --- | --- |
| Mupen64Plus core | https://github.com/mupen64plus/mupen64plus-core / `3be6336a1de6f32ccc4f57bf80c988163fe6bf7a` | GPL-2.0-or-later in source headers; see upstream LICENSES | R4300 interpreter, RDRAM, peripheral devices, scheduling, saves |
| CXD4 RSP | https://github.com/mupen64plus/mupen64plus-rsp-cxd4 / `00906a92641c540a64e4e7505012157315314810` | CC0 for RSP; retain per-file API/OSAL notices | Scalar LLE RSP, graphics and audio microcode |
| SDL2 or SDL2-compat | https://github.com/libsdl-org/SDL / https://github.com/libsdl-org/sdl2-compat | zlib | Required by upstream core; not the native UI, video, input or audio backend |
| SDL3 (when using SDL2-compat) | https://github.com/libsdl-org/SDL | zlib | Explicitly bundled dlopen dependency |
| libpng | https://www.libpng.org/pub/png/libpng.html | libpng | Upstream screenshot support |
| zlib | macOS system library | zlib | Upstream compressed saves; supplied by macOS |

The core's embedded minizip, MD5 and CIC code retain their original notices.
OSD is disabled; OGLFT, FreeType, the Bitstream font and OpenGL are not bundled.
The application, adapters and build glue are GPL-2.0-or-later. The independent
`Sources/AurumRDP` module and original test-ROM generator are MIT. No renderer
code has been copied from another emulator. GPL API headers stay on the adapter
side of the interface, not inside AurumRDP.

## Auditable core extension

`patches/mupen64plus-core/0001-interpreter-host-poll.patch` adds a callback every
4096 interpreter dispatches. Host commands then run on the core-owning thread,
including stop when a ROM never configures VI. VI callbacks also service the
mailbox because upstream idle-loop acceleration may advance one VI per dispatch.
No cross-thread writes to the upstream non-atomic stop flag are needed.

The patch applies to a `git archive` export in `.build/sources`, never the
submodule checkout. Its hash is part of the build manifest and object-cache key.
This is a temporary integration extension, not a claimed upstream API. A future
ARM64 dynarec and non-yielding RSP need separate cancellation integration. Move
long-lived upstream changes to a dedicated fork when their scope stabilizes.

The packager collects dependency licenses and records exact dylib source hashes,
source locations and submodule commits in `Resources/BuildManifest.json`.
Homebrew build libraries are **not yet source-pinned**; the manifest makes each
local artifact traceable, but does not claim bit-for-bit reproducible builds.
A release must provide corresponding source, including upstream components,
build scripts and any modifications, alongside its binaries. Ad-hoc signing is
for local testing, not Developer ID signing/notarization or an App Store release.

## Evaluated alternatives

- [paraLLEl-RDP](https://github.com/Themaister/parallel-rdp): MIT, Vulkan compute;
  upstream describes comparison against Angrylion-Plus. Appropriate architecture
  reference and future differential-test target. No Vulkan/MoltenVK dependency
  or direct source port in this milestone.
- [Orbit64](https://github.com/Wronk4/orbit64): repository has no top-level LICENSE
  at inspection; GitHub license metadata is empty. Do not copy implementation
  without establishing a clear applicable license. Metal accuracy was not tested.
- [ares](https://github.com/ares-emulator/ares): active multi-system architecture;
  ISC main code with a composite third-party LICENSE. Valuable accuracy reference,
  but a larger integration surface than Mupen's established plugin ABI.
- [simple64](https://github.com/simple64/simple64): GPL-3.0 project, useful LLE /
  paraLLEl integration reference. Not selected as a UI or binary dependency.

API metadata observed core pushed 2026-10-21, ares 2026-10-22, Orbit64 2026-10-14,
simple64 2025-02-14 and paraLLEl-RDP 2024-11-09. These timestamps indicate repository
activity only, not correctness, support guarantees or emulator compatibility.
