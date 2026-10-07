"""Report whether VaultCraft's shared mapping is live, and who is talking on it.

    python tools/check_link.py

The phase 0 check: run it while Fallout: New Vegas is running and it should show a live game
heartbeat. Reads only.

Note that this deliberately does *not* use Python's mmap(tagname=...). That calls CreateFileMapping,
which **creates** the mapping when it does not already exist, so a naive check would manufacture an
undersized placeholder with the right name and then report nonsense. Worse, if such a script were
still running when the game started, the game's CreateFileMapping would find that stub, succeed with
ERROR_ALREADY_EXISTS, and then fail to map the 191 MB it asked for. OpenFileMappingW only ever
opens, never creates.
"""

import ctypes
import struct
import sys
import time

MAGIC = 0x43594B53  # "SKYC"
NAME = "Local\\SkyCraft_v1"
FILE_MAP_READ = 0x0004

OFF_HEADER = 0x0
HEADER_SIZE = 0x20
OFF_SKY_STATE = 0x100
SKY_STATE_SIZE = 0x40

k32 = ctypes.windll.kernel32
k32.GetTickCount64.restype = ctypes.c_uint64
k32.OpenFileMappingW.restype = ctypes.c_void_p
k32.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                             ctypes.c_size_t]
k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]


def tick():
    return k32.GetTickCount64()


def read(view, offset, size):
    return ctypes.string_at(view + offset, size)


def main():
    handle = k32.OpenFileMappingW(FILE_MAP_READ, False, NAME)
    if not handle:
        print(f"no mapping named {NAME}")
        print("Either the game is not running, or VaultCraft.dll failed to load.")
        print("Check VaultCraft.log next to FalloutNV.exe, and that the DLL is beside the exe.")
        return 1

    view = k32.MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0)
    if not view:
        k32.CloseHandle(handle)
        print("mapping exists but could not be mapped")
        return 1

    try:
        magic, version, game_pid, mc_pid, game_beat, mc_beat = struct.unpack(
            "<IIIIQQ", read(view, OFF_HEADER, HEADER_SIZE))
        now = tick()

        print(f"mapping   {NAME}")
        print(f"magic     0x{magic:08X} {'OK' if magic == MAGIC else 'WRONG - not a VaultCraft mapping'}")
        print(f"version   {version}")

        def age(beat):
            if beat == 0:
                return "never"
            ms = now - beat
            return f"{ms / 1000:.1f}s ago" + ("" if ms < 5000 else "   (STALE)")

        print(f"game      pid {game_pid}, last heartbeat {age(game_beat)}")
        print(f"minecraft pid {mc_pid}, last heartbeat {age(mc_beat)}")

        try:
            seq, flags, world_id, epoch, x, y, z, yaw, pitch, tseq, vw, vh, hour = struct.unpack(
                "<IIIIdddffIIIf", read(view, OFF_SKY_STATE, SKY_STATE_SIZE))
            print(f"state     seq {seq}{' (MID-WRITE)' if seq & 1 else ''} flags 0x{flags:X} "
                  f"world 0x{world_id:08X} epoch {epoch}")
            print(f"position  {x:.2f} {y:.2f} {z:.2f}   yaw {yaw:.1f} pitch {pitch:.1f}")
            print(f"viewport  {vw}x{vh}   game hour {hour:.2f}")
        except (OSError, struct.error) as e:
            print(f"state     unavailable ({e})")
    finally:
        k32.UnmapViewOfFile(ctypes.c_void_p(view))
        k32.CloseHandle(ctypes.c_void_p(handle))

    ok = magic == MAGIC and game_beat != 0 and (now - game_beat) < 5000
    print()
    print("LINK OK - the plugin is loaded and heartbeating." if ok else "LINK NOT OK - see above.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())