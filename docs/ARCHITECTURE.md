# Integration baseline

## Ownership

The native app links a small C ABI runtime. A single emulation thread owns all
Mupen APIs, plugin lifetimes, emulated RDRAM, the RDP decoder and Metal renderer.
UI input writes atomic button/mute state; UI commands enter a mutex-protected
mailbox. Snapshot and frame-copy functions never return pointers into emulated
memory. The frame snapshot is copied under a mutex.

The interpreter host hook and VI callback service pending commands. Pausing parks
the core thread on a condition variable, which stop/resume wake. Core/plugin
startup errors unwind completed startup steps on the same thread. Reopening a
ROM joins the previous worker before constructing a new session. The GUI handles
quit asynchronously rather than blocking the main run loop on a join.

The singleton API assumes `aur_start` and `aur_wait` have one controlling caller;
the GUI uses its main actor for both. Plugin callbacks and command submission may
occur on other threads only as documented by the C header.

## RDP and memory consistency

The video adapter consumes host-order words from Mupen's word-swapped memory,
handling both RDRAM and XBUS/DMEM command sources. The independent decoder buffers
partial commands; unsupported operations are counted rather than silently claimed
as supported. SyncFull sets the DP interrupt. Timing/counter behavior remains a
known incomplete area requiring reference tests.

Metal fill dispatches have one invocation per 32-bit word to avoid races between
two 16-bit pixel writes. Each fill copies current RDRAM to a shared Metal buffer,
completes GPU execution, and copies memory back **before** returning to the CPU.
VI similarly snapshots RDRAM and completes scanout before publishing host pixels.
This is intentionally conservative and expensive. It avoids unsynchronized GPU
writes into CPU-owned memory; it is not a zero-copy or optimized design.

The video plugin owns a persistent 4 KiB TMEM store, cleared on new ROM/reset.
LoadTile/Block/TLUT callbacks use the same bounded uploader as diagnostic replay.
TextureRectangle/Flip snapshots carry their issue-time tile, scissor and raw
OtherModes. The texture path accepts restricted integer-edge RGBA16 and CI4/CI8
copy rectangles with an RGBA16 TLUT. A CPU planner validates the entire draw,
maps each qword's S/T coordinates and groups destinations by host word. The plan
contains only addresses and CI4 nibble/palette-port selectors; a Metal kernel
reads TMEM, performs indexed palette lookup where needed, applies the optional
one-bit alpha comparison and writes both framebuffer lanes. The planner merges
words shared by neighboring rows or odd framebuffer bases, so each GPU invocation
has exclusive ownership. Unsupported draws leave RDRAM untouched. Completion and
CPU readback precede subsequent uploads, which allows a rendered framebuffer to
become a texture source. Point-cycle indexed sampling, other TLUT types, other
indexed formats and filtering remain unsupported.

The MetalKit view uses the same compiled presentation shaders as the offscreen
test. Its single shared texture is not updated until the previous GPU read has
completed. Main-thread waits are an explicit temporary limitation, to be replaced
by bounded in-flight resources after correctness testing. No Vulkan, MoltenVK,
OpenGL or SDL window is used for rendering.

`aurum-angrylion-reference` is a separate development tool whose original GPL
adapter replays the same `.aurcap` command/VI records through the pinned
MAME-licensed Angrylion core. It owns synthetic RDRAM/DMEM and register arrays,
performs no network or ROM access, and is never linked into the application. Its
layer output is a development artifact, not a claim of hardware truth. It emits
the final RDRAM bytes in Angrylion's host-word-swapped layout, one compressed
hidden/coverage byte per 16-bit pixel, the complete 4096-byte TMEM image, and
direct VI color/depth/coverage RGBA8 frames for each captured VI event. Each RDP
submission and VI event also emits a checkpoint with neutral record/submission/
command/VI coordinates and a stable hash over Angrylion's comparable RDP state
subset. The manifest names each layout; `aurum-reference-diff` reports the first
differing checkpoint, byte/pixel coordinate, and expected/actual value. Checkpoint
hashes identify state divergence, while layer files preserve the follow-on state
needed when shrinking a failing capture.

The CPU-only `aurum-aurcap-replay` emits the same checkpoint and RDRAM/VI-color
layer format for Aurum's supported fill path. Its current RDRAM result matches
Angrylion on the generated fill fixture, including the submitted command bytes.
Its direct VI layer follows the reference's active-area semantics (packed
H/V start bounds, x/y scale dimensions, and filtered border crop) and now
matches the 205x240 RGBA8 frames pixel-for-pixel. Verified uploads emit TMEM;
missing hidden/depth/
coverage layers are reported as unavailable by the diff tool.

`aurum-metal-replay` shares the capture reader, state checkpoints and diagnostic
VI-color export, but dispatches fill/copy through the production Metal renderer.
It has no software texture-draw fallback. Scalar replay still reports texture
draws as unsupported. Independent differential fixtures check all framebuffer
bytes and TMEM, including a draw-to-upload dependency within one submission.
The neutral v1 state summary remains a comparison subset, not a full save state.

## Audio and controls

CXD4 executes audio microcode in LLE; the Core Audio adapter receives AI PCM DMA.
A bounded single-producer/single-consumer ring feeds AudioQueue. Full buffers drop
excess samples rather than block the core; empty buffers output silence. This is
a plumbing baseline, not final pacing/resampling. Sample-rate changes rebuild the
queue. Runtime RDRAM size bounds audio DMA reads, including 4 MiB configurations.

The first GameController extended gamepad and focused native keyboard view supply
player 1. The core handles non-raw controller pak protocols. UI focus loss clears
keys. Native settings replace SDL keybindings, which are disabled in the core.

## Deliberate omissions

- CPU dynarec, RSP vector differential tests and cancellation inside an infinite RSP task.
- Full RDP, hidden coverage/depth state, remaining texture formats/filtering,
  VI filtering/interlace and accurate DP scheduling.
- Portable save states: core state alone omits the external decoder/renderer state.
- Multiple controllers, remapping, rumble, VRU, netplay and 64DD.
- Hardened-runtime signing, notarization, updater and public release pipeline.

These are tracked work, not fallbacks that report success. Unknown graphics commands
produce a visible warning. There is no claim that a booting commercial ROM renders
correctly. Original diagnostics avoid unlicensed content and proprietary boot code.
