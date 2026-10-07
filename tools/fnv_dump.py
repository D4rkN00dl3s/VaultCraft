#!/usr/bin/env python3
"""
Dump FalloutNV.exe's *decrypted* .text out of a running game and rebuild a PE Ghidra can read.

Fallout: New Vegas ships with its .text section encrypted at rest and decrypts it into memory at
load, so the on-disk exe is useless for code RE (measured: entropy 8.000, 1 prologue in 12.4 MB).
This captures the module image from a live process and splices the decrypted bytes back into a
copy of the original file. Headers, .rdata and the relocation table stay exactly as shipped.

Usage:
    python fnv_dump.py                      # wait for the game, dump 45s after it appears
    python fnv_dump.py --delay 90           # wait longer (slow disks / big saves)
    python fnv_dump.py --game <exe path>    # non-Steam / different folder
    python fnv_dump.py --out <dir>          # where to write (default: alongside the script)

It needs the game running and past its initial load. You do NOT need a save loaded: .text is
decrypted before the main menu appears, so any post-boot moment works. Same-user access only.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import math
import os
import struct
import sys
import time
from collections import Counter

TH32CS_SNAPMODULE = 0x00000008
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
INVALID_HANDLE_VALUE = -1

# Handles are pointer-sized. Without these restypes ctypes defaults to c_int and truncates them
# on 64-bit, which shows up as "access denied" or a bogus snapshot handle.
k32 = ctypes.windll.kernel32
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("th32ModuleID", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("GlblcntUsage", wt.DWORD),
        ("ProccntUsage", wt.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_ubyte)),
        ("modBaseSize", wt.DWORD),
        ("hModule", wt.HMODULE),
        ("szModule", wt.WCHAR * 256),
        ("szExePath", wt.WCHAR * 260),
    ]


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wt.DWORD),
        ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD),
        ("szExeFile", wt.WCHAR * 260),
    ]


def find_pid(name):
    """Every running process whose image name matches, by CreateToolhelp32Snapshot on processes."""
    TH32CS_SNAPPROCESS = 0x00000002
    snap = ctypes.windll.kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap in (0, INVALID_HANDLE_VALUE):
        return []
    out, e = [], PROCESSENTRY32()
    e.dwSize = ctypes.sizeof(PROCESSENTRY32)
    ok = ctypes.windll.kernel32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            out.append(e.th32ProcessID)
        ok = ctypes.windll.kernel32.Process32NextW(snap, ctypes.byref(e))
    ctypes.windll.kernel32.CloseHandle(snap)
    return out


def find_module(pid, name):
    """Base address and size of the main module, which is the only one matching the exe's name."""
    e = MODULEENTRY32()
    e.dwSize = ctypes.sizeof(MODULEENTRY32)
    snap = ctypes.windll.kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    if snap in (0, INVALID_HANDLE_VALUE):
        raise OSError("CreateToolhelp32Snapshot failed: %d" % ctypes.GetLastError())
    try:
        ok = ctypes.windll.kernel32.Module32FirstW(snap, ctypes.byref(e))
        while ok:
            if e.szModule.lower() == name.lower() or e.szExePath.lower().endswith("\\" + name.lower()):
                return ctypes.cast(e.modBaseAddr, ctypes.c_void_p).value, e.modBaseSize, e.szExePath
            ok = ctypes.windll.kernel32.Module32NextW(snap, ctypes.byref(e))
    finally:
        ctypes.windll.kernel32.CloseHandle(snap)
    raise OSError("module %s not found in pid %d" % (name, pid))


def read_mem(hproc, addr, size):
    buf = (ctypes.c_ubyte * size)()
    got = ctypes.c_size_t()
    if not ctypes.windll.kernel32.ReadProcessMemory(
            hproc, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        raise OSError("ReadProcessMemory failed at 0x%x: %d" % (addr, ctypes.GetLastError()))
    if got.value != size:
        raise OSError("short read at 0x%x: %d of %d" % (addr, got.value, size))
    return bytes(buf)


def entropy(d):
    if not d:
        return 0.0
    c = Counter(d)
    n = len(d)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


def sections_of(pe_bytes):
    pe = struct.unpack_from("<I", pe_bytes, 0x3C)[0]
    n = struct.unpack_from("<H", pe_bytes, pe + 6)[0]
    opt_size = struct.unpack_from("<H", pe_bytes, pe + 20)[0]
    off = pe + 24 + opt_size
    out = []
    for i in range(n):
        h = off + i * 40
        name = pe_bytes[h:h + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", pe_bytes, h + 8)
        chars = struct.unpack_from("<I", pe_bytes, h + 36)[0]
        out.append(dict(name=name, vsz=vsz, va=va, rsz=rsz, rptr=rptr, chars=chars))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=r"C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas\FalloutNV.exe")
    ap.add_argument("--proc", default="FalloutNV.exe")
    ap.add_argument("--delay", type=int, default=45, help="seconds to wait after the process appears")
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--all-sections", action="store_true",
                    help="also take .data/.rdata from memory (only safe when not relocated)")
    ap.add_argument("--wait-timeout", type=int, default=900,
                    help="give up waiting for the process after this many seconds")
    args = ap.parse_args()

    if not os.path.exists(args.game):
        sys.exit("game exe not found: %s" % args.game)
    on_disk = open(args.game, "rb").read()
    pref_base = struct.unpack_from("<I", on_disk, struct.unpack_from("<I", on_disk, 0x3C)[0] + 24 + 28)[0]

    print("waiting up to %ds for %s ..." % (args.wait_timeout, args.proc))
    pids = []
    waited = 0
    while not pids and waited < args.wait_timeout:
        pids = find_pid(args.proc)
        if not pids:
            time.sleep(2)
            waited += 2
    if not pids:
        sys.exit("gave up after %ds; %s never started" % (args.wait_timeout, args.proc))
    pid = pids[0]
    started = time.time()
    print("found pid %d; waiting %ds for the load to finish" % (pid, args.delay))
    time.sleep(args.delay)

    # Re-resolve: the process may have restarted, and a new base is cheap to re-read.
    pids = find_pid(args.proc)
    if pid not in pids:
        sys.exit("process %d is gone; it probably crashed. Re-run." % pid)
    base, size_of_image, exe_path = find_module(pid, args.proc)
    print("module base 0x%08x, size 0x%x, path %s" % (base, size_of_image, exe_path))

    h = ctypes.windll.kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        sys.exit("OpenProcess failed: %d" % ctypes.GetLastError())
    try:
        head = read_mem(h, base, 0x1000)
        if head[:2] != b"MZ":
            sys.exit("no MZ at module base")
        pe = struct.unpack_from("<I", head, 0x3C)[0]
        magic = struct.unpack_from("<H", head, pe + 24)[0]
        if magic != 0x10B:
            sys.exit("expected PE32, got optional magic 0x%x" % magic)
        live_size = struct.unpack_from("<I", head, pe + 24 + 56)[0]
        dll_chars = struct.unpack_from("<H", head, pe + 24 + 70)[0]
        dynamic_base = bool(dll_chars & 0x0040)
        print("live SizeOfImage 0x%x  DllCharacteristics 0x%x  DYNAMIC_BASE=%s"
              % (live_size, dll_chars, dynamic_base))
        print("preferred base 0x%08x  loaded at 0x%08x  relocated=%s"
              % (pref_base, base, base != pref_base))
        image = read_mem(h, base, live_size)
    finally:
        ctypes.windll.kernel32.CloseHandle(h)

    # Splice memory over the file image. .text is code and carries no absolute self-relocations,
    # so it is always safe to take. Everything else is only safe when the image did not move.
    take_all = args.all_sections and base == pref_base
    out = bytearray(on_disk)
    report = []
    for s in sections_of(on_disk):
        if s["rsz"] == 0:
            continue
        use_mem = s["name"] == ".text" or take_all
        if not use_mem:
            continue
        src = base + s["va"]
        n = min(s["rsz"], s["vsz"]) if s["vsz"] else s["rsz"]
        if src + n > base + live_size:
            n = max(0, base + live_size - src)
        if n <= 0:
            continue
        out[s["rptr"]:s["rptr"] + n] = image[s["va"]:s["va"] + n]
        file_before = on_disk[s["rptr"]:s["rptr"] + n]
        file_after = bytes(out[s["rptr"]:s["rptr"] + n])
        report.append((s["name"], n, entropy(file_before), entropy(file_after),
                       file_before.count(b"\x55\x8b\xec"), file_after.count(b"\x55\x8b\xec")))

    dst = os.path.join(args.out, "FalloutNV.decrypted.exe")
    open(dst, "wb").write(bytes(out))
    print("\nwrote %s" % dst)
    print("  sha256 %s" % hashlib.sha256(bytes(out)).hexdigest())
    print("\nsection   bytes     entropy before -> after   prologues before -> after")
    for name, n, e0, e1, p0, p1 in report:
        print("  %-8s %9d   %.3f -> %.3f          %7d -> %7d"
              % (name, n, e0, e1, p0, p1))

    text = next((r for r in report if r[0] == ".text"), None)
    if not text:
        sys.exit("no .text in the output -- something went wrong")
    if text[5] < 1000:
        print("\nWARNING: only %d prologues in the dumped .text. Expected many thousands."
              % text[5])
        print("The dump was probably taken before .text was decrypted, or the game is not the")
        print("one you patched. Try again with a longer --delay.")
    else:
        print("\nOK: %d function prologues recovered. This looks like real, decrypted code."
              % text[5])
    print("elapsed %.0fs" % (time.time() - started))


if __name__ == "__main__":
    main()