"""Read the collision ring's head and tail, and decode whatever Minecraft has not consumed.

    python tools/col_ring.py

The ring is the only way to tell a working collision stream from a broken one, and the log cannot:
the plugin logs that it sent a patch, which says nothing about whether Minecraft read it. Head is
what we have written, tail is what Minecraft has consumed, so the gap between them is the queue.

Read-only, and it opens the mapping with OpenFileMappingW rather than mmap(tagname=...) - see the
note in check_link.py for why creating the mapping would be actively harmful.
"""

import ctypes
import struct
import sys

MAGIC = 0x43594B53
NAME = "Local\\SkyCraft_v1"
OFF_COLLISION_RING = 0x20000
RING_BYTES = 32 << 20
HEAD_OFF = 0x00
TAIL_OFF = 0x40
DATA_OFF = 0x80
DATA_BYTES = RING_BYTES - DATA_OFF

COL_CLEAR, COL_REGION, COL_TRIS = 1, 2, 3

k32 = ctypes.windll.kernel32
k32.GetTickCount64.restype = ctypes.c_uint64
k32.OpenFileMappingW.restype = ctypes.c_void_p
k32.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
k32.MapViewOfFile.restype = ctypes.c_void_p
k32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                              ctypes.c_size_t]


def align8(n):
    return (n + 7) & ~7


def main():
    handle = k32.OpenFileMappingW(4, False, NAME)
    if not handle:
        print(f"no mapping named {NAME}; start the game")
        return 1
    view = k32.MapViewOfFile(handle, 4, 0, 0, 0)
    if not view:
        print("mapping exists but could not be mapped")
        return 1

    base = OFF_COLLISION_RING
    head = struct.unpack_from("<Q", ctypes.string_at(view + base + HEAD_OFF, 8))[0]
    tail = struct.unpack_from("<Q", ctypes.string_at(view + base + TAIL_OFF, 8))[0]

    print(f"ring     head {head}  tail {tail}  queued {head - tail} bytes")
    if head == 0:
        print("         nothing has been written yet")
        return 1
    if head == tail:
        print("         fully consumed - Minecraft is reading the ring")

    pos = tail % DATA_BYTES
    data = base + DATA_OFF
    shown = 0
    print("pending messages:")
    while pos < head and shown < 6:
        msg_type, payload = struct.unpack_from("<II", ctypes.string_at(view + data + pos, 8))
        if msg_type == 0:
            print(f"  pad at {pos} - skip to start of ring")
            pos += DATA_BYTES - pos
            continue
        if msg_type == COL_CLEAR:
            (epoch,) = struct.unpack_from("<I", ctypes.string_at(view + data + pos + 8, 4))
            print(f"  CLEAR epoch {epoch}")
        elif msg_type in (COL_REGION, COL_TRIS):
            r = struct.unpack_from("<iiiiiiII", ctypes.string_at(view + data + pos + 8, 32))
            name = "TRIS" if msg_type == COL_TRIS else "REGION"
            print(f"  {name} box ({r[0]},{r[1]},{r[2]})..({r[3]},{r[4]},{r[5]}) "
                  f"epoch {r[6]} count {r[7]}")
            if msg_type == COL_TRIS and r[7]:
                # First triangle's heights, which is the quickest sanity check that the vertices
                # are plausible MC blocks rather than FNV units or garbage.
                v = struct.unpack_from("<9f", ctypes.string_at(view + data + pos + 8 + 32, 36))
                print(f"        first tri v0({v[0]:.1f},{v[1]:.1f},{v[2]:.1f}) "
                      f"v1({v[3]:.1f},{v[4]:.1f},{v[5]:.1f}) v2({v[6]:.1f},{v[7]:.1f},{v[8]:.1f})")
        else:
            print(f"  unknown type {msg_type} at {pos}")
        pos += align8(8 + payload)
        shown += 1

    if shown == 0 and head != tail:
        print("  (queued bytes do not decode - head and tail disagree)")
    return 0


if __name__ == "__main__":
    sys.exit(main())