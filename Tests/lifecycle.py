#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run in a subprocess with an external timeout; never load two runtime copies."""
import ctypes as C
import pathlib
import struct
import sys
import time

root = pathlib.Path(sys.argv[1]).resolve()
app = pathlib.Path(sys.argv[2]).resolve() / "Contents"
lib = C.CDLL(str(app / "Frameworks/libaurum-runtime.dylib"))
class Snapshot(C.Structure):
    _fields_ = [("state", C.c_int), ("width", C.c_uint32), ("height", C.c_uint32),
                ("frames", C.c_uint64), ("rdpCommands", C.c_uint64),
                ("unsupported", C.c_uint64), ("audioFrames", C.c_uint64), ("message", C.c_char * 512)]
lib.aur_start.argtypes = [C.c_char_p] * 4 + [C.c_uint64]
lib.aur_snapshot.argtypes = [C.POINTER(Snapshot)]
lib.aur_command.argtypes = [C.c_int]
lib.aur_set_buttons.argtypes = [C.c_uint32]
lib.aur_get_buttons.restype = C.c_uint32
lib.aur_get_rdram_size.restype = C.c_uint32
lib.aur_copy_frame.argtypes = [C.POINTER(C.c_uint32), C.c_size_t, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
lib.aur_copy_frame.restype = C.c_size_t
STOP, PAUSE, RESUME, SAVE, LOAD, RESET = range(6)

def snapshot():
    result = Snapshot()
    lib.aur_snapshot(C.byref(result))
    return result
def until(predicate, label):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        state = snapshot()
        if predicate(state):
            return state
        time.sleep(0.01)
    raise AssertionError(f"Timed out: {label}; state={state.state} frames={state.frames} message={state.message!r}")
def start(path, limit=0):
    return lib.aur_start(str(path).encode(), str(app / "Frameworks").encode(),
                         str(app / "Resources").encode(), str(root / "user-lifecycle").encode(), limit)
def stop():
    lib.aur_command(STOP)
    until(lambda s: s.state in [0, 5], "stop")
    assert lib.aur_wait() == 0

paths = [str(root / "fill.z64").encode(), str(app / "Frameworks").encode(),
         str(app / "Resources").encode(), str(root / "user-lifecycle").encode()]
for index in range(4):
    for invalid_path in [None, b""]:
        arguments = paths.copy()
        arguments[index] = invalid_path
        assert lib.aur_start(*arguments, 4) == 0, "null/empty path accepted"
        assert snapshot().state == 0, "invalid path changed the idle session"

assert start(root / "missing.z64") == 1
until(lambda s: s.state == 5, "missing ROM rejected")
assert lib.aur_wait() == 1
invalid = root / "invalid.z64"
invalid.write_bytes(bytes(4096))
assert start(invalid) == 1
until(lambda s: s.state == 5, "bad ROM signature rejected")
assert lib.aur_wait() == 1
source = (root / "fill.z64").read_bytes()
for extension, group in [("v64", 2), ("n64", 4)]:
    swapped = b"".join(source[i:i+group][::-1] for i in range(0, len(source), group))
    path = root / f"fill.{extension}"
    path.write_bytes(swapped)
    assert start(path, 4) == 1
    assert lib.aur_wait() == 0
    assert snapshot().rdpCommands == 6 and snapshot().audioFrames == 1024
for iteration in range(2):
    assert start(root / "fill.z64", 4) == 1
    assert lib.aur_wait() == 0
    assert snapshot().frames >= 4 and snapshot().audioFrames == 1024
# Previously saved HLE settings must be overridden before InitiateRSP reads them.
config = root / "user-lifecycle/mupen64plus.cfg"
text = config.read_text().replace("DisplayListToGraphicsPlugin = False", "DisplayListToGraphicsPlugin = True")
text = text.replace("AudioListToAudioPlugin = False", "AudioListToAudioPlugin = True")
config.write_text(text.replace("DisableExtraMem = False", "DisableExtraMem = True"))
assert start(root / "fill.z64", 4) == 1
assert lib.aur_wait() == 0 and lib.aur_get_rdram_size() == 4 * 1024 * 1024
pixels = (C.c_uint32 * (320 * 240))()
width, height = C.c_uint32(), C.c_uint32()
assert lib.aur_copy_frame(pixels, len(pixels), C.byref(width), C.byref(height)) == len(pixels)
assert pixels[0] == 0xFF00FF00, "persisted HLE configuration bypassed the RSP"
config.write_text(config.read_text().replace("DisableExtraMem = True", "DisableExtraMem = False"))
assert start(root / "fill.z64") == 1
until(lambda s: s.frames >= 3, "running")
assert start(root / "fill.z64") == 0, "concurrent session accepted"
lib.aur_set_buttons(0x50B00080)
assert lib.aur_get_buttons() == 0x50B00080
lib.aur_command(PAUSE)
paused = until(lambda s: s.state == 3, "pause").frames
lib.aur_command(PAUSE)
time.sleep(0.05)
assert snapshot().state == 3 and snapshot().frames == paused
time.sleep(0.1)
assert snapshot().frames == paused, "frames advanced while paused"
lib.aur_command(RESUME)
until(lambda s: s.frames > paused, "resume")
lib.aur_command(RESUME)
time.sleep(0.05)
assert snapshot().state == 2, "repeated resume changed the running state"
lib.aur_command(RESET)
until(lambda s: s.audioFrames >= 2048, "reset re-ran test ROM")
lib.aur_command(PAUSE)
until(lambda s: s.state == 3, "second pause")
stop()

# Original infinite CPU loop with no VI setup: the host hook must still service stop.
rom = bytearray((root / "fill.z64").read_bytes())
rom[0x40:] = bytes(len(rom) - 0x40)
struct.pack_into(">2I", rom, 0x40, 0x1000FFFF, 0)
(root / "no-vi.z64").write_bytes(rom)
assert start(root / "no-vi.z64") == 1
until(lambda s: s.state == 2, "no-VI core started")
time.sleep(0.1)
assert snapshot().frames == 0
stop()
print("PASS: ROM byte orders, recovery, repeated sessions, 4 MiB RDRAM, forced LLE, input, pause/resume/reset, no-VI stop")
