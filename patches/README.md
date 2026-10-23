# Upstream patch policy

Keep submodule checkouts unchanged. Record each upstream modification here as a
reviewable patch against the exact gitlink commit. `scripts/prepare_core.py`
exports that commit into an ignored build directory, verifies the patch, then
applies it. Do not patch a user's checkout or download untracked replacement code.

Current core patch: `mupen64plus-core/0001-interpreter-host-poll.patch`.

- Adds one optional C function-pointer setter and periodic interpreter callback.
- No emulated register, instruction semantics or plugin ABI changes.
- Configured while the core is stopped; callbacks execute on its worker thread.
- Handles host commands before VI initialization; VI also handles commands for
  accelerated idle loops. Tested with an original no-VI infinite CPU loop.
- Does not claim cancellation inside an infinite RSP task or a future dynarec.

Long-lived extensions should migrate to a dedicated fork with upstreamable commits.
