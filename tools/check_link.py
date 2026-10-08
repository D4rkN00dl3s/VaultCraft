"""Report whether VaultCraft's shared mapping is live, and who is talking on it.

    python tools/check_link.py          # one snapshot
    python tools/check_link.py --watch  # follow it, to see it move

The phase 0 check: run it while Fallout: New Vegas is running and it should show a live game
heartbeat. Reads only.

--watch exists because the plugin's whole job now is publishing a moving position, and a single
snapshot cannot distinguish "the player is standing still" from "the publisher has stopped". It
re-reads once a second and marks the row when the position or yaw changed since the last row.

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
OFF_MC_STATE = 0x200
# Only the head of McState is needed: seq, flags, position, angles. eyeHeight and the rest follow.
MC_STATE_HEAD = "<IIdddff"
MC_STATE_HEAD_SIZE = struct.calcsize(MC_STATE_HEAD)

MC_IN_WORLD = 1 << 0
MC_ON_GROUND = 1 << 2
MC_SNEAKING = 1 << 3
MC_SPRINTING = 1 << 4
MC_FLYING = 1 << 7

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


def age(beat, now):
    """How long ago a heartbeat was written, flagged once it looks stale."""
    if beat == 0:
        return "never"
    ms = now - beat
    return f"{ms / 1000:.1f}s" + ("" if ms < 5000 else " STALE")


def main():
    return watch() if "--watch" in sys.argv else snapshot()


def open_mapping():
    """Maps the shared region read-only. Returns (handle, view) or (None, None).

    Deliberately uses OpenFileMappingW, which only ever opens. Python's mmap(tagname=...) would
    create a wrongly-sized stub under the real name if the game were not running, and that stub
    then breaks the game's own MapViewOfFile of the 191 MB it asked for.
    """
    handle = k32.OpenFileMappingW(FILE_MAP_READ, False, NAME)
    if not handle:
        return None, None
    view = k32.MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0)
    if not view:
        k32.CloseHandle(ctypes.c_void_p(handle))
        return None, None
    return ctypes.c_void_p(handle), ctypes.c_void_p(view)


def snapshot():
    handle, view = open_mapping()
    if not handle:
        print(f"no mapping named {NAME}")
        print("Either the game is not running, or VaultCraft.dll failed to load.")
        print("Check VaultCraft.log next to FalloutNV.exe, and that the DLL is beside the exe.")
        return 1

    try:
        magic, version, game_pid, mc_pid, game_beat, mc_beat = struct.unpack(
            "<IIIIQQ", read(view.value, OFF_HEADER, HEADER_SIZE))
        now = tick()

        print(f"mapping   {NAME}")
        print(f"magic     0x{magic:08X} {'OK' if magic == MAGIC else 'WRONG - not a VaultCraft mapping'}")
        print(f"version   {version}")

        print(f"game      pid {game_pid}, last heartbeat {age(game_beat, now)}")
        print(f"minecraft pid {mc_pid}, last heartbeat {age(mc_beat, now)}")

        try:
            seq, flags, world_id, epoch, x, y, z, yaw, pitch, tseq, vw, vh, hour = struct.unpack(
                "<IIIIdddffIIIf", read(view.value, OFF_SKY_STATE, SKY_STATE_SIZE))
            print(f"state     seq {seq}{' (MID-WRITE)' if seq & 1 else ''} flags 0x{flags:X} "
                  f"world 0x{world_id:08X} epoch {epoch}")
            print(f"position  {x:.2f} {y:.2f} {z:.2f}   yaw {yaw:.1f} pitch {pitch:.1f}")
            print(f"viewport  {vw}x{vh}   game hour {hour:.2f}")
        except (OSError, struct.error) as e:
            print(f"state     unavailable ({e})")

        # What Minecraft decided. This is the half that answers "is the puppet working": the game
        # publishes where FNV's player is, but MC publishes where MC's physics put its player, and
        # the plugin moves FNV's to match. If this stays at the origin, Minecraft has nothing to
        # stand on and the loop has nothing to do.
        try:
            mseq, mflags, mx, my, mz, myaw, mpitch = struct.unpack(
                MC_STATE_HEAD, read(view.value, OFF_MC_STATE, MC_STATE_HEAD_SIZE))
            if mseq == 0:
                print("mc        no state yet (Minecraft has not written)")
            else:
                flags = []
                if mflags & MC_IN_WORLD:
                    flags.append("inWorld")
                if mflags & MC_ON_GROUND:
                    flags.append("ON-GROUND")
                if mflags & MC_SNEAKING:
                    flags.append("sneaking")
                if mflags & MC_SPRINTING:
                    flags.append("sprinting")
                if mflags & MC_FLYING:
                    flags.append("flying")
                print(f"mc        seq {mseq}{' (MID-WRITE)' if mseq & 1 else ''} "
                      f"flags 0x{mflags:X} {' '.join(flags) or '-'}")
                print(f"mc pos    {mx:.2f} {my:.2f} {mz:.2f}   yaw {myaw:.1f} pitch {mpitch:.1f}")
                if not (mflags & MC_ON_GROUND):
                    print("          note: not on the ground - MC has no floor here, so it will not move")
        except (OSError, struct.error) as e:
            print(f"mc        unavailable ({e})")
    finally:
        k32.UnmapViewOfFile(view)
        k32.CloseHandle(handle)

    ok = magic == MAGIC and game_beat != 0 and (now - game_beat) < 5000
    print()
    print("LINK OK - the plugin is loaded and heartbeating." if ok else "LINK NOT OK - see above.")
    return 0 if ok else 1


def watch():
    """Follow the published state, marking each row that changed. Ctrl-C to stop."""
    handle, view = open_mapping()
    if not handle:
        print(f"no mapping named {NAME}; start the game first")
        return 1
    print(f"{'time':>6}  {'beat':>7}   sky pos (FNV's view)   yaw pitch |   mc pos (MC's view)   ground")
    print("        'beat' is how long ago the plugin ticked. sky is what FNV publishes; mc is what")
    print("        Minecraft's physics decided, which is what FNV is then moved to match.")
    previous = None
    try:
        while True:
            now = tick()
            _, _, _, mc_pid, game_beat, mc_beat = struct.unpack(
                "<IIIIQQ", read(view.value, OFF_HEADER, HEADER_SIZE))
            seq, flags, _, _, x, y, z, yaw, pitch, _, _, _, _ = struct.unpack(
                "<IIIIdddffIIIf", read(view.value, OFF_SKY_STATE, SKY_STATE_SIZE))
            mseq, mflags, mx, my, mz, myaw, _ = struct.unpack(
                MC_STATE_HEAD, read(view.value, OFF_MC_STATE, MC_STATE_HEAD_SIZE))
            moved = previous is not None and (round(x, 3), round(y, 3), round(z, 3), round(yaw, 1)) != previous
            previous = (round(x, 3), round(y, 3), round(z, 3), round(yaw, 1))
            ground = "yes" if mflags & MC_ON_GROUND else ("no" if mseq else "-")
            mcpos = f"{mx:8.2f} {my:7.2f} {mz:8.2f}" if mseq else "     (none)"
            print(f"{time.strftime('%H:%M:%S')}  {age(game_beat, now):>7}  "
                  f"{x:8.2f} {y:8.2f} {z:8.2f} {yaw:4.0f} {pitch:5.0f} | "
                  f"{mcpos}  {ground}{'' if not moved else '  <-'}")
            time.sleep(1.0)
    except KeyboardInterrupt:
        print()
    finally:
        k32.UnmapViewOfFile(view)
        k32.CloseHandle(handle)
    return 0


if __name__ == "__main__":
    sys.exit(main())