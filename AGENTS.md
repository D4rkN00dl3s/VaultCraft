# AGENTS.md

Play Fallout: New Vegas as a Minecraft player. Minecraft runs hidden with its own physics,
inventory, HUD and combat; the game runs its world, NPCs, quests and saves; the two talk over
shared memory. Neither game is rewritten.

Early and experimental. Not affiliated with Bethesda, Obsidian or Mojang. You need to own both
games. **Back up your saves.**

## Layout

| | |
|---|---|
| `fabric/` | the Minecraft Fabric mod (Java 25, MC 26.3, Fabric API). Ported from SkyCraft, near-verbatim. |
| `protocol/skycraft_protocol.h` | the shared-memory byte layout both halves follow. **Single source of truth.** |
| `nvse/` | the xNVSE plugin (C++, x86). Our code. |
| `nvse/extern/NVSE/` | xNVSE source as a git submodule, for its engine headers only. Do not edit. |
| `tools/` | the memory dumper and live-mapping probes. |

## Build

Prereqs: Visual Studio 2026 (Desktop C++, **x86 toolset**), CMake 3.25+, JDK 25, Git.

```bat
cd fabric
gradlew build
gradlew test                    ;; JUnit: SkyRayTest, TriColliderTest (pure Java, fast)
gradlew runClient               ;; dev Minecraft; stays up when the game exits
```

The plugin builds separately, and has no dependency on the mod:

```bat
cd nvse
cmake --preset default
cmake --build --preset release
```

Pass `--no-configuration-cache` to every `gradlew` call, the way `tools/` scripts do, even though
`gradle.properties` enables the cache.

`VAULTCRAFT_DEPLOY_DIR` (set by the `default` preset to the Fallout: New Vegas folder) copies the
built DLL next to `FalloutNV.exe` after each build, but only if that folder already exists. **xNVSE
is not a virtual-filesystem mod manager**: the plugin DLL sits beside the exe, not in `Data\` and
not in an MO2 profile. A failed copy is a warning, not a build error — a running game holds the
DLL.

The build is `Win32`, not `x64`, and `CMakeLists.txt` fails configuration if you get it wrong.
Fallout: New Vegas is a 32-bit game; a 64-bit plugin cannot load into it.

`nvse/src/*.cpp` is globbed with `CONFIGURE_DEPENDS`, so new files are picked up without
re-running configure.

### Three macros that will bite you

xNVSE's headers are old and assume a very specific preprocessor environment. `CMakeLists.txt`
comments each one, but the failure modes are so indirect they are worth restating. All three were
found the hard way, and **none produces an error that names its cause.**

| Do not define | Because | Symptom if you do |
|---|---|---|
| `WIN32_LEAN_AND_MEAN` | it stops `windows.h` including `rpc.h`, the only route to `rpcndr.h`'s `typedef unsigned char byte`. Their headers use a bare `byte` 36 times and never declare it. | `unknown type "byte"`, then dozens of **bogus** `static assertion failed` errors about struct sizes that are actually fine |
| `NOMINMAX` | their headers call bare `min()`/`max()`, which only resolve because `windows.h` defines them as macros | `'max': identifier not found` |
| `EDITOR` **at all** | not even `EDITOR=0`. They use `#ifdef EDITOR` alongside `#if RUNTIME`, so `EDITOR=0` still satisfies the `#ifdef` and compiles the GECK-only `EditorData` member into the runtime layout | every `BaseFormComponent` descendant comes out exactly **0x10 bytes too large**: `TESForm` 0x28 not 0x18, `BGSTextureSet` 0xB0 not 0xA0, and so on |

That last one produces a wall of misleading errors, because a wrong base-class size breaks every
derived class's `static_assert`. **If struct sizes start failing, suspect the macros before
suspecting the structs.**

Because `min`/`max` are live macros, write any `std::min` or `std::max` in our own code as
`(std::min)(a, b)`.

### Include order, and exports

`src/PCH.h` includes `nvse/prefix.h` and uses theirs rather than hand-rolling one: it is the root
of NVSE's include graph, supplying the primitive types, `winsock2` before `Windows.h`, and the
`UnorderedMap` alias that `CommandTable.h` needs without including it. The standard library goes
*between* the prefix and `PluginAPI.h`, because `PluginAPI.h` reaches `CommandTable.h`, which
uses `std::string` and `std::vector` itself. Get that order wrong and the parse collapses into the
same misleading struct-size errors.

`extern "C"` alone does **not** export from a DLL. Both entry points need
`__declspec(dllexport)`: `PluginManager.cpp` finds them with `GetProcAddress`, and
`PluginChecker.cpp` probes for `NVSEPlugin_Query` to decide what a DLL even is. Without it the
plugin builds cleanly and then fails to load with no useful diagnostic.

### Logging

`src/Log.cpp` is ours, not NVSE's `IDebugLog`. That header looks header-only but is not: it needs
`common/IDebugLog.cpp`, which needs `IFileStream.cpp`, which needs `IDataStream.cpp` and
`IErrors.cpp`. Dragging NVSE's common library in to write a log line was not worth it, so we have
about forty lines of our own that also mirror to `OutputDebugString` for DebugView.

## Changing the protocol

`protocol/skycraft_protocol.h` is hand-mirrored in `fabric/src/main/java/dev/skycraft/link/Proto.java`.
**Change both, and bump `kVersion`, in the same commit.** Nothing catches a one-sided edit at
compile time: the failure is a runtime "protocol mismatch" at handshake, which looks like a
startup bug rather than a version skew.

The mapping is `Local\SkyCraft_v1`. Skyrim creates it; Minecraft opens it.

All multi-byte values are little-endian, fixed-size structs, no packing. Coordinates in the
protocol are always Minecraft space (blocks, Y up, Z south) even when they describe the game's
world.

## Fallout: New Vegas specifics

These are the facts that are expensive to rediscover. Do not assume them; they were measured.

**The on-disk `FalloutNV.exe` cannot be reverse engineered.** Its `.text` section is encrypted at
rest — measured entropy 8.000, one function prologue in 12.4 MB — and the game decrypts it into
memory at load. This is stock FNV, not patch damage: the pre-patch `FalloutNV_backup.exe` is
identical in this respect. `.rdata`, `.data` and the relocation table are *not* encrypted, so
strings and imports can be read straight off disk.

Get code out of the game, never off disk:

```bat
python tools\fnv_dump.py          :: waits for the game, dumps, splices a decrypted PE
```

Run the game, let it finish loading (no save needed — `.text` decrypts before the main menu),
then run the dumper. It reads the live module and writes `FalloutNV.decrypted.exe`, which Ghidra
imports normally. The image loads at its preferred base `0x00400000` with `DYNAMIC_BASE` clear, so
**addresses in the dump are the addresses the running game uses** — hooks need no fixups.

The dumper only splices `.text`. `.bind` stays encrypted, so the 4 GB patcher's trampolines there
are not analysable; use `--all-sections` if you ever need them.

**Havok is BSHavok, statically linked into the exe.** `bhkVisualDebugger.cpp` and
`TES4\Havok\SDK\Include\` are compiled into `.rdata`. Havok RTTI names are findable as strings:
`hkpShapeCollection @ 0x010d3a60`, `hkpWorldCinfo @ 0x010cb20c`, `hkpSimulation @ 0x010d42b4`.

`FUN_00c68230` is the BSHavok **shape factory registration** and is the best map of the physics
object model in the binary — it registers every shape class by name with its factory. Terrain
collision is `bhkNiTriStripsShape` (factory `FUN_00ca6670`) and/or `bhkMoppBvTreeShape`. Referencing
an RTTI name string gets you the data table, not code; go through the factory.

Havok's offset fields read as 32-bit in this build. Evidence, not proof: BSHavok's registration
and dispatch code is uniformly `undefined4 *` with 32-bit vtable calls. If a shape walk ever
produces impossible addresses, re-examine whether the field is 64-bit before assuming a logic bug.

**Rendering is Direct3D 9**, not 11 (`D3D9.DLL`, `D3DX9_38.DLL` in the imports). Input arrives
through DirectInput 8, and xNVSE already exposes raw versus post-filter state plus a key-disable
API in `nvse/extern/NVSE/nvse/nvse/Hooks_DirectInput8Create.h`, so input bridging is easier here
than the Skyrim version was.

**There is no Address Library.** Fallout: New Vegas has no equivalent of Skyrim's relocation IDs,
so every hook is a raw address found by hand against this specific build. Keep them in one table
in `src/Engine.cpp`, not scattered, and expect them to break under engine-mutating mods.

Engine access goes through `src/Port.h` and only through it. That boundary is what keeps the
Minecraft half free of Fallout specifics and makes Skyrim recoverable later as a second
implementation rather than a fork.

## Conventions

- Fabric uses split source sets: `src\main` is both sides (the integrated server plus client),
  `src\client` is client-only. Rendering belongs in `src\client`; gameplay rules and server-side
  code in `src\main`. Each needs a matching entry in its mixin config json.
- The package root stays `dev.skycraft` and the mod id `skycraft`, carried over from SkyCraft so
  the port diffs against it cleanly. If you rename them, rename the Fabric mod id, both mixin
  configs, and the `data/skycraft/` resource paths together.
- C++20, MSVC `/W4 /permissive-`. Java 25.
- Logging: the plugin goes through spdlog to `VaultCraft.log` in the xNVSE log directory. The mod
  uses its own slf4j logger.
- Comments in this repo are plain and direct, with no filler. Match that.

## Verifying changes

- `gradlew test` is the only automated test suite, and it is pure Java: `SkyRayTest` and
  `TriColliderTest`. There is no C++ test target.
- Everything else is checked against the running game. Use `tools/`: `dump_collision.py`,
  `probe_feet.py`, `tick_monitor.py` and `trace_motion.py` read the live shared mapping
  read-only and are safe to run while the game is up. `dump_collision.py` is the quickest way to
  confirm the game side is actually streaming terrain.