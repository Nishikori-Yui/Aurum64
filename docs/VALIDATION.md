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
- **Hardware accuracy:** fills, VI conversion and RGBA16 texture uploads are
  compared byte-for-byte against Angrylion-rdp-plus. Nothing has been compared
  against captured N64 hardware, and fractional bounds, unsupported RDP modes and
  VI edge cases remain unverified. See the 2026-10-24 record below.
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

# Texture upload record - 2026-10-24

## Completed locally

| Check | Actual result |
| --- | --- |
| RGBA16 TMEM addressing | Verified against `aurum-angrylion-reference` at commit `9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc` |
| RGBA32 LoadTile split | Verified: the high halfword lands in bank 0 and the low halfword in bank 1 at the same word index, with the same column order as RGBA16 |
| LoadTile layout | The odd-row dword swap, a non-zero `tmem` base, line strides 1/2/4 and TMEM bank 1 all match the reference |
| LoadBlock layout | `sh` values 4, 12, 32 and 44 with `dxt=0` match; four halfwords are written per edge-walker step |
| LoadTLUT layout | Single-row loads of 4 and 7 entries with a non-zero `tmem` base match; each entry occupies four hardware words in bank 0 |
| Byte-exact TMEM | The `texture` fixture reports `"status":"match"` with `tmem` among the checked layers, 4096 of 4096 bytes identical |
| Negative control | Removing the odd-row swap makes the same fixture report `"status":"differ"` at TMEM byte 24 |
| Upload bounds | TMEM destination overflow via the line stride, columns wider than the stride, and RDRAM source overflow are all rejected, and a rejected upload leaves TMEM unmodified |
| Opcode accounting | LoadTLUT, LoadBlock and LoadTile are reported through `unsupported` when no handler models them; they are no longer silent no-ops |

Five fixtures cover this, all generated by `scripts/make_reference_fixtures.py`
and compared by `scripts/test.py`: `texture` (mixed LoadTile, LoadTile into bank
1, LoadBlock and LoadTLUT), `texture_stride` (line strides 1 and 4, non-zero
`tmem` base), `texture_block` (`sh` 4, 12 and 44), `texture_tlut` (4 and 7
entries, non-zero `tmem` base) and `texture_rgba32` (the bank split with line
strides 2 and 4 and a non-zero `tmem` base). Each reports `"status":"match"` with
`tmem` among the checked layers. All fixtures are built from original commands
only.

Negative controls confirm the fixtures are not vacuous matches. Each of these
injected defects is caught with `"status":"differ"` on the `tmem` layer, and all
three were reverted afterwards: dropping the odd-row dword swap, writing one
halfword per edge-walker step instead of four, and exchanging the two RGBA32
banks.

## Deliberate divergence from the reference

- **Out-of-range source reads:** `rdram_read_idx32()` masks the index into RDRAM
  and substitutes zero when it falls outside the configured size. The uploader
  rejects such an upload with `false`, so it surfaces as unsupported instead of
  returning plausible data. No fixture depends on the aliasing behavior, so both
  engines still agree byte-for-byte on every comparison above.
- **RGBA32 at an address that is not word aligned:** rejected by the uploader.
  The reference still uploads it, writing 61 of the 62 TMEM bytes that an aligned
  upload produces. An offset of 4 bytes is word aligned and is accepted: the
  `texture_rgba32_offset` fixture confirms it matches the reference byte-for-byte
  with `tmem` among the checked layers, and a negative control that tightens the
  guard to 8-byte alignment makes that fixture report the upload as unsupported.
  Offsets of 2 and 6 bytes were checked during development with throwaway
  captures, are rejected, and are not part of the committed suite. This gap is
  deliberate and narrow rather than silently approximated.

## Not validated / not complete

- **TMEM formats beyond RGBA16 and RGBA32:** LoadTile is modeled for `ti_size` 2
  and 3. CI4/CI8 + TLUT, IA4/8/16, I4/8 and YUV layouts, including CI4 packing
  and the YUV conversion coefficients, are reported as unsupported rather than
  approximated.
- **RGBA32 LoadBlock:** reported as unsupported. The reference writes
  `steps * 4 - 2` texels rather than a whole number per step, and that layout is
  not verified yet.
- **LoadTile into TMEM bank 1 with RGBA32:** rejected. Only bank 0 has been
  verified for the split layout, so the unverified half of the window is refused.
- **LoadBlock into TMEM bank 1:** rejected for the same reason.
- **LoadBlock with non-zero DXT:** still rejected. Row advance and odd/even row
  exchange are not modeled.
- **Hidden buffer, VI depth and VI coverage layers:** the scalar replay does not
  emit these, so the diff tool still lists them as unavailable.
- **Texture sampling:** no fetch path consumes TMEM yet, so upload correctness is
  not yet exercised by a rendered pixel.

# Capture memory record - 2026-10-24

## Completed locally

| Check | Actual result |
| --- | --- |
| `kRecordMemory` consumer | Implemented in both the scalar replay and the reference harness, sharing one helper in `Capture.cpp` so the two cannot drift apart |
| Byte mapping | A guest byte at N64 address `A` is stored at host address `A ^ 3`, matching the word-swapped layout of the `.rdram` layer |
| Ground truth | The mapping is asserted against the fill path, which was validated against the reference before memory records existed, not against the two engines agreeing with each other |
| Bounds | A record that runs past the end of RDRAM is rejected and leaves RDRAM unmodified |

Two fixtures cover this: `texture_memory_record` writes four rows of RGBA16
texels with a memory record and uploads them with LoadTile, and
`texture_memory_record_fill` writes the same texels with fills.
`scripts/test.py` compares the reference RDRAM image of the two over the source
region and fails if they differ.

Negative controls confirm both checks are load bearing. Changing the mapping to
`A ^ 2` makes the unit test fail and makes the two fixtures' reference images
differ, and restoring the previous write-while-validating loop makes the
rejected-record test fail because RDRAM keeps a partial write.

## Not validated / not complete

- **No producer yet.** Nothing in the emulator emits a memory record, so the
  path is exercised only by fixtures. Capturing CPU stores would be needed to
  record real 8-bit texture sources.
- **8-bit texel formats.** CI4, CI8, IA and I uploads are still unimplemented.
  The memory record was a prerequisite because a fill cannot place an 8-bit
  texel exactly, and the first probe at CI8 produced a source buffer that the
  replay's fill path had mislaid, so its TMEM output was not meaningful.

# 8-bit texel uploads - 2026-10-24

## Completed locally

| Check | Actual result |
| --- | --- |
| CI8, IA8, I8 LoadTile | Verified against `aurum-angrylion-reference`; the upload layout is shared because it depends on the texture image size, not the format |
| 8-bit placement rule | One byte per texel at `(tmem + line * row) << 3`, with even rows reversing the bytes of each aligned word and odd rows additionally swapping the two halves of the 8-byte group |
| Row base movement | Covered with a non-zero `tmem` base and line strides of 1, 2 and 4 |
| Row stride bound | A tile wider than eight bytes per line unit is rejected |
| Byte-exact TMEM | Four fixtures report `"status":"match"` with `tmem` among the checked layers |
| Negative control | Removing the odd-row swap fails the unit test and makes `texture_ci8` and `texture_8bit_offset` report `"status":"differ"` at TMEM bytes 16 and 32 |

These fixtures depend on the capture memory record, because a fill cannot place
an 8-bit texel exactly.

## Not validated / not complete

- **4-bit texels (CI4, I4):** rejected. The reference marks
  `PIXEL_SIZE_4BIT` as a crashed pipeline and writes nothing, so there is no
  layout to compare against. Reported as unsupported rather than approximated.
- **8-bit LoadBlock and LoadTLUT:** rejected, neither layout is verified.
  LoadTLUT entries are 16-bit regardless of the texel format, so the existing
  RGBA16 TLUT path is what a CI texture would use.
- **8-bit texel meaning:** only the upload placement is verified. How CI
  indices combine with a TLUT, and the IA and I channel layouts, are sampling
  concerns and are not exercised yet.
- **YUV:** not implemented. Needs its own conversion coefficients and a
  different `tmem_formatting` path in the reference.

# Explicit texture fetch - 2026-10-24

Historical limitation: these tests checked reference TMEM placement but used
VI-style RGBA16 channel expectations. Direct reference texel comparisons on
2026-10-26 found and corrected that decoding error; see the record below.

## Completed locally

| Check | Actual result |
| --- | --- |
| RGBA16 point fetch | Decodes a texel from reference TMEM with the RGBA5551 channel rules the VI scanout already applies |
| RGBA32 point fetch | Recombines the two banks into one 32-bit texel and decodes it with the RGBA8888 rules |
| Ground truth | `scripts/test.py` runs the fetch test against the `.tmem` layers that `aurum-angrylion-reference` produced, so the sampler is checked against hardware bytes rather than against the uploader that wrote them |
| Addressing | Uses the same `tileWordIndex` as the uploader, plus a row-base range check against the 9-bit unit field |
| Negative control | Replacing the fetch index with a variant that drops the odd-row swap fails when the reference layers are supplied |

Two fixtures carry exactly one clean upload each, `sampler_rgba16` and
`sampler_rgba32`. The combined texture fixtures stack uploads that overwrite
each other's TMEM, so a texel there cannot be attributed to one upload; the test
matrix requires TMEM, sampling and framebuffer to be compared separately for
that reason.

The negative control also shows why the reference layers matter: with no
directory argument the test still passes, because the remaining cases only check
the uploader and the sampler against each other.

## Not validated / not complete

- **Nothing renders yet.** The sampler has no consumer in the RDP path, so no
  pixel output exercises it and TextureRectangle and Flip are not reached.
- **8-bit and 4-bit texels:** rejected by the fetch. Turning a CI index into a
  color needs TLUT lookup, and IA and I need their channel layouts.
- **Coordinates:** no mask, mirror, clamp, shift, signed values or wrap, so only
  texels inside the uploaded rectangle resolve.
- **Filtering:** point sampling only. Three-point filtering, LOD, detail and
  sharpen are separate work and must not receive a nearest sample silently.

# Point/copy texture coordinates - 2026-10-26

## Completed locally

The independent reference remains Angrylion-Plus at
`9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc`. A development-only
`--texture-coordinates input output` mode calls its actual coordinate and
`fetch_texel` functions. Requests contain original SetTile/SetTileSize words
and signed fixed-point S/T values; the separate Aurum executable decodes those
words into rectangle snapshots and compares coordinates and texels.
No reference library is linked into the production renderer or application.

| Check | Actual result |
| --- | --- |
| Coordinate comparison | 1,279,760 requests matched point S/T and copy S[0..3]/T, seven integer outputs per request |
| Signed inputs and shift | All 65,536 input bit patterns at all 16 shifts, on both axes; negative fractions, 16-bit truncation and sign extension included |
| Bounds and addressing modes | All 16 masks and shift values, mirror/clamp combinations, fractional and reversed tile bounds, mask/mirror saturation at ten bits, and 10,000 seeded combinations |
| Point vs copy | Point mapping clamps when enabled or mask=0; copy ignores clamp and increments S lanes after shift/origin conversion, before each lane's mask/mirror |
| Point sampling | 977,243 accepted RGBA16/32 fetches matched the reference's actual texel channels from a shared deterministic 4 KiB TMEM image |
| RGBA16 correction | RGB channels now replicate five bits to eight, and alpha becomes 0/255. Texture fetch is distinct from the existing VI scanout quantization |
| Unsupported fetches | Non-RGBA formats, negative or overflowing addresses and unmodeled TMEM wrap remain explicitly rejected |
| Decoder state | LoadTile, LoadBlock and LoadTLUT update tile bounds even without an accepting upload handler; later changes leave captured rectangles unchanged |
| Negative controls | Corrupting a mapped coordinate or texel channel makes the comparison fail; a truncated probe is rejected |
| Test isolation | CTest fixtures generate requests, run the independent reference process, then compare. Selecting only `texture-coordinates` also brings in its prerequisites |

Full `scripts/build.sh` and `MTL_DEBUG_LAYER=1 python3 scripts/test.py` both
exited 0. CTest passed 11/11. The test output directory started empty; the run
also passed lifecycle checks, eight optimized Swift path startups, relocated
bundle/dyld/signature checks, capture/replay and 20 matching layer comparisons
(including XBUS wrap). A 200-byte runtime capture stayed within its budget and
retained its terminal record. Local logs: `.build/m4-coordinate-build.log`,
`.build/m4-coordinate-test.log`, and
`.build/native/Testing/Temporary/LastTest.log`.

The empty-directory run also verifies a correction to the preceding audit
tests: they now copy freshly generated `reference/fill.*` artifacts rather than
depend on a previous run's `reference/actual` directory. That dependency caused
the earlier PR #2 head `278b860` to fail on clean CI.

## Scope boundary

This closes the signed, non-perspective point/copy coordinate slice of M4-05.
The copy results are coordinates for a four-lane fetch, not a copy-cycle
framebuffer implementation. TextureRectangle/Flip still report unsupported
in production. Copy qword fetching, rectangle rasterization, combiner/blender,
perspective and LOD, three-point filtering, and CI/IA/I/YUV sampling remain
separate work. Neither this software-reference match nor a successful build
establishes game compatibility or hardware accuracy.
