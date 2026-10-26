# Dependencies and source provenance

Checked on 2026-10-24. Build inputs are submodule commits, not floating branches.

| Component | Source / pin | License | Use |
| --- | --- | --- | --- |
| Mupen64Plus core | https://github.com/mupen64plus/mupen64plus-core / `3be6336a1de6f32ccc4f57bf80c988163fe6bf7a` | GPL-2.0-or-later in source headers; see upstream LICENSES | R4300 interpreter, RDRAM, peripheral devices, scheduling, saves |
| CXD4 RSP | https://github.com/mupen64plus/mupen64plus-rsp-cxd4 / `00906a92641c540a64e4e7505012157315314810` | CC0 for RSP; retain per-file API/OSAL notices | Scalar LLE RSP, graphics and audio microcode |
| Angrylion-Plus core | https://github.com/ata4/angrylion-rdp-plus / `9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc` | MAME license for `src/core`; Mupen64Plus plugin layer is GPL-2.0 and excluded | Development-only offline software reference |
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

## M2 software-reference audit

The M2 accuracy investigation was completed on 2026-10-24 before copying any
candidate source. After the decision, the selected pinned core was added as a
submodule and remains excluded from the application bundle.

| Candidate | Exact pin | License finding | macOS ARM64 result | Decision |
| --- | --- | --- | --- | --- |
| [angrylion-rdp-plus](https://github.com/ata4/angrylion-rdp-plus) | `9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc` | `src/core`, `src/output`, and their headers are covered by the repository's `MAME License.txt`; `CREDITS.txt` attributes the original RDP to angrylion/MESS contributors. The separate Mupen64Plus plugin layer carries GPL-2.0 headers. | Configured and built with `-DBUILD_MUPEN64PLUS=OFF -DBUILD_PROJECT64=OFF -DCMAKE_OSX_ARCHITECTURES=arm64`. Both static libraries were arm64; no source patch was needed. CMake still discovers OpenGL because the output library is part of the project, but the RDP/VI core has no emulator plugin dependency. One existing `-Wswitch` warning appeared. | Selected M2 software reference. Use `src/core` behind an original adapter and keep the Mupen64Plus plugin sources out of the build. |
| Themaister's older Angrylion fork | `31bdb1f0a79dd726017a38432540c6b5db0fa117` (pinned by paraLLEl-RDP) | Same MAME license and credits; this predates the core/output split. | Modern CMake rejects its `cmake_minimum_required(VERSION 2.8)`. With `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` and arm64 settings, `libalp-core.a` built without changing source. The project also unconditionally builds its Mupen64Plus plugin and needs OpenGL. | Do not integrate. Retain only as the exact Angrylion version used by paraLLEl's comparison tooling. |
| [paraLLEl-RDP](https://github.com/Themaister/parallel-rdp) | `1cecd042b2619bc505c12bfdc713808386f2b54d` | Root and RDP implementation files are MIT. Pinned Granite submodule `cf71dee71fb00110749a9a3fcbd87b0297e49b6a` is MIT; the pinned Angrylion submodule above is MAME-licensed; bundled Vulkan profiles are Apache-2.0. | Not built in this milestone: its runner is Vulkan compute, so macOS requires an external Vulkan/MoltenVK runtime and a substantially larger dependency surface. | Secondary architecture and conformance-design reference. Any future executable must stay isolated from the production app. |
| [ares](https://github.com/ares-emulator/ares) | `c76aaffa6e69778baf9a66d1da04298ac2ed7d1d` | Composite licensing; the relevant N64 subsystem requires file-by-file review rather than applying one project-wide label. | Not built for this audit. | Optional cross-reference only. Re-audit exact files before any use. |
| [Orbit64](https://github.com/Wronk4/orbit64) | `93879b6a642ab321b1ad9f25646412193e29aeb2` | No LICENSE/COPYING/NOTICE file and GitHub reports no recognized license at this pin. | Not applicable. | Do not copy or link until an applicable license is established. |

The selected reference remains a development-only, offline process. Its MAME
license forbids commercial distribution, so it must not be bundled with a
production `.app` or marketed as a product feature. A distributed development
build must provide the pinned upstream source, all local modifications, and the
upstream MAME notice; original adapter code cannot be labeled MIT on behalf of
upstream files. Reference output files are test data generated from original
ROMs and do not redistribute upstream source.

The 2026-10-26 texture-copy implementation studies the pinned reference's
qword fetch, copy rasterization and coordinate behavior. Aurum's address-only CPU
plan and host-word-owned Metal kernel are original implementations; no upstream
renderer bodies were transplanted. Original fixture generation and separate
reference execution verify the resulting RDRAM/TMEM bytes. Angrylion remains
excluded from the production app.

## Evaluated alternatives

- [paraLLEl-RDP](https://github.com/Themaister/parallel-rdp): MIT/Vulkan compute
  with MIT Granite and MAME Angrylion submodules; see the exact pins above.
  Appropriate architecture reference and future differential-test target. No
  Vulkan/MoltenVK dependency or direct source port in this milestone.
- [Orbit64](https://github.com/Wronk4/orbit64): no applicable license was found
  at the inspected pin. Do not copy implementation without establishing a clear
  license. Metal accuracy was not tested.
- [ares](https://github.com/ares-emulator/ares): active multi-system architecture
  with composite third-party licensing. Valuable accuracy cross-reference, but a
  larger integration surface than the selected standalone core.
- [simple64](https://github.com/simple64/simple64): GPL-3.0 project, useful LLE /
  paraLLEl integration reference. Not selected as a UI or binary dependency.

Repository activity observed 2026-10-24: core 2026-10-21, ares 2026-10-22,
Orbit64 2026-10-14, simple64 2025-02-14, paraLLEl-RDP 2024-11-09, and
angrylion-rdp-plus 2025-01-05. These timestamps indicate repository activity
only, not correctness, support guarantees or emulator compatibility.
