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

The real Minecraft side runs under Prism Launcher, not the vanilla launcher: the Fabric instance is
`C:\Users\mart\AppData\Roaming\PrismLauncher\instances\26.3\minecraft`. `runClient` is still the
fast loop for the mod alone; the Prism instance is what pairs with a running game.

Pass `--no-configuration-cache` to every `gradlew` call, the way `tools/` scripts do, even though
`gradle.properties` enables the cache.

`VAULTCRAFT_DEPLOY_DIR` (set by the `default` preset to the Fallout: New Vegas folder) copies the
built DLL into `Data\NVSE\Plugins\` after each build, creating it if needed. A failed copy is a
warning, not a build error — a running game holds the DLL.

**That folder is not a guess.** xNVSE loads its *core* DLLs (`nvse_1_4.dll` and friends) from the
game root by name, but plugins it scans exactly one directory, and nowhere else:

```cpp
m_pluginDirectory = falloutDirectory + "Data\\NVSE\\Plugins\\";   // PluginManager.cpp:548
for (IDirectoryIterator iter(m_pluginDirectory.c_str(), "*.dll"); ...)  // :688
```

A plugin dropped in the game root is silently ignored: it builds, deploys, never loads, and writes
no log to explain why. That is the shape of the failure, so it is worth recognising.

`RegisterListener` takes a **sender**, and the sender must be `"NVSE"`:

```cpp
PluginManager::Dispatch_Message(0, NVSEMessagingInterface::kMessage_MainGameLoop, ...);
// ...which walks:
for (auto iter = s_pluginListeners[sender].begin(); ...)   // sender == 0
```

`LookupHandleFromName` maps `"NVSE"` to handle 0, and slot 0 is the only one anything dispatches
into. Registering under our own plugin name files the listener in our own slot, where nothing ever
reaches it: the plugin loads, logs its startup lines, and then goes deaf. The plugin looks healthy
and receives nothing.

`kMessage_MainGameLoop` is the per-frame tick, dispatched from `HandleMainLoopHook`, which NVSE
patches at `0x0086B386`. That is where the heartbeat belongs.

The plugin writes `VaultCraft.log` to the **game root**, not to `Data\NVSE\Plugins`, so it sits
alongside `FalloutNV.exe`. It opens with `_wfsopen(..., _SH_DENYWR)` rather than `fopen`, because
`fopen` takes exclusive access and nothing could read the log until the game exited.

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

**Two ways this file used to crash the game.** Both are worth knowing, because neither names its
cause and both produced a fault inside `ucrtbase.dll`:

- **`_wfopen_s` mode must be plain `"w"`.** Any `ccsb=` token — `UTF-8`, `65001`, or a bare number
  — is rejected by this UCRT's parameter validation, which raises the invalid-parameter handler and
  `__fastfail`s with `0xc0000409`. Measured on Windows 10.0.26100: `w` and `wb` work, every
  `ccsb` variant kills the process. Because opening the file is the first thing *every* log line
  does, the symptom is a crash with **no log file at all** — the one thing that would have told you.
- **The logger is narrow-only.** Passing a `wchar_t*` to `%s`, or a `const char*` to `%ls`, makes
  the same argument validation fire the same way. Narrow wide strings with `log::Narrow()` at the
  call site rather than reaching for `%ls`.

A bad format string in a plugin is not a logged error, it is a dead game, so treat the two as
hazards rather than style issues.

### Debugging a plugin crash without launching the game

`vclink_test` (CMake target `vclink_test`) runs the link and logging code in a console:

```bat
cmake --build --preset release --target vclink_test
build\RelWithDebInfo\vclink_test.exe
```

It creates the mapping, heartbeats, and prints what it saw, exiting 0 on success. Every fault in
`Link` and `Log` so far has reproduced here immediately, where a crash inside Fallout: New Vegas
takes minutes and gives you only an event-log entry. Both of the bugs above were found by making
the game crash first and the harness print it second.

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

**Havok's offset fields read as 32-bit in this build.** Settled, not merely argued: Havok object
sizes come out of the factory constructors and none exceeds 32 bits of meaningful extent, and every
BSHavok dispatch we have read is `undefined4 *` with 32-bit vtable calls. No 64-bit pointer traffic
anywhere in the range `0x00b00000`–`0x00e00000`, which holds 9,529 of the exe's 63,261 functions.

### The Havok spike: what is established, and the dead ends

The binary has no symbols and BSHavok is undecorated, so methods are all `FUN_xxxxxx` and there is
nothing to grep for. Identification works two ways: BSHavok's own source paths and assert strings are
linked in, and each class's factory is reachable from `FUN_00c68230`.

**583 class names recovered** from `.rdata`, spanning the whole engine — `bhkWorld`, `hkpPhysicsSystem`,
`bhkCharacterProxy`, `hkpTriSampledHeightFieldBvTreeShape`, `hkPackedNiTriStripsData`, and so on. FNV
drives its player with a Havok **character proxy**, which is worth knowing for the phase 2 puppet.

**FNV's terrain is *not* `bhkNiTriStripsShape`.** Measured in a live cell, by scanning the heap for
every known class vtable and counting:

| Class | Objects in one loaded cell |
|---|---|
| `bhkRigidBody` | 2112 |
| `bhkBlendCollisionObject` | 811 |
| **`bhkMoppBvTreeShape`** | **502** |
| **`bhkPackedNiTriStripsShape`** | **501** |
| `bhkLimitedHingeConstraint` | 213 |
| `hkPackedNiTriStripsData` | 169 |
| `bhkMalleableConstraint` | 112 |
| `bhkAabbPhantom` | 19 |
| **`bhkNiTriStripsShape`** | **0** |

Zero for `bhkNiTriStripsShape` is the useful result: scanning for it alone finds nothing and looks
identical to a mistake. Static RE had pointed at it; the measurement overruled that.

**Scanning technique:** Havok object identity *is* the vtable, so one pass over committed memory
identifies every class at once. Skip `MEM_IMAGE` — the vtables themselves live there and would match
everything — and range-check each dword against the vtable span before looking it up, which is what
keeps a full pass over the heap to a few hundred ms. `Engine.cpp` is this probe, kept as a diagnostic
rather than deleted.

**35 class vtables recovered** from `FUN_00c68230`, which registers each class by pushing its name and
its factory; the factory stores the vtable at offset 0 last, after the base-class vtable. The method
needs the factory to *exist* in Ghidra's view — pick the nearest preceding `push` of a code address
inside the registration function, then take the last `.rdata` address the factory stores. That
independently reproduced `0x010C771C` for `bhkNiTriStripsShape`, matching what its constructor says,
which is the cross-check that the derivation is right.

Nine factories yielded no vtable — `bhkBoxShape`, `bhkSphereShape`, `bhkCapsuleShape`,
`bhkCylinderShape`, `bhkTriangleShape`, `bhkConvexVerticesShape`, `bhkConvexTransformShape`,
`bhkConvexListShape`, `bhkCollisionObject` among them. Characters and props use exactly those convex
shapes, so this needs fixing before phase 2.

**Bounds are at `+0x60`–`+0x7C`** on the `0x010C755C` wrapper: eight floats, read out as min/max by
`FUN_00ca37e0`, which is that class's AABB accessor. Enough for coarse culling without vertex data.
Do **not** read those offsets on a `bhkMoppBvTreeShape` — different class, and doing so yields a
float of 136164352.0, which is not a bound.

### The dead end, and why it matters

Several runs went into decoding what turned out to be the **source asset file, not runtime vertices**:

```
bhkMoppBvTreeShape +0x08  ->  vtable 0x010CA330, which is a Gamebryo asset loader
                              (its vtable is followed by the literal string
                               "_FallOut_3\Platforms\")
                                +0x14 -> vtable 0x0102E368, a 0x30-byte data container
                                +0x18 -> the packed blob
```

`0x0102E368` has three destructors and refcounting and nothing else — a plain POD container, no
interpreter. The blobs read as `7BFF841F`, `AAAAAAAA`, `000690D6` and are **not** a proprietary Havok
encoding; they are unparsed `.msGame` bytes that the loader is still holding after Havok has already
built its MOPP tree from them.

Runtime triangles therefore live inside the MOPP structure, which is a compressed BVH — the genuinely
hard case, and the one the plan's fallback exists for. **Phase 1 decided: ray-cast, do not decode the
MOPP.** That retires the question rather than deferring it.

Ray-casting is cheaper here than the plan assumed, because shapes are found by scanning and need no
physics world: 1,003 per cell, each with a known vtable and a known AABB. Candidate entry point is
`FUN_00d21450(this, in, out, ctx)` in the `0x010C755C` vtable — the `(this, input, output, context)`
shape a Havok `rayCast` has. What it still needs is the `hkpRayCastInput` / `hkpRayCastOutput` layouts.

**A physics-system object is partly mapped** (unused so far, but it is how a `hkpWorld` would be
reached): `FUN_00cd24d0(this, world)` stores the world at `this + 0x04`, and `FUN_00cd2530(this,
world)` appends it to the array at `this + 0x6C` (count `+0x70`, capacity `+0x74`). Also present: four
listener arrays at `+0x5C`–`+0x68`, another at `+0x84`/`+0x88`, and a world-context array at
`+0x78`/`+0x7C` with a stride of **`0xF0`** — `sizeof(hkpWorldContext)`, and `hkpWorldCinfo` is
referenced from the same function.

### Traps that each cost a run

- **Timing.** Terrain collision is built *during* cell load. A scan fired on `kMessage_PostLoadGame`
  ran in the same millisecond as the message and found nothing. Scanning 5 s later, four times, found
  4,450 objects on the first pass and nothing new after — the cell is fully built well before then.
- **Counters must reset per pass.** They were `static`, so pass 2 reported exactly double pass 1 and
  it read like the world was still streaming in. It was not; the same objects were counted twice.
- A global "dump the first N objects found" budget is useless: the scan walks memory in address order,
  so whichever class sits lowest consumes all of it. Dump by class name instead.
- **Dereference pointers before dumping them.** `DumpWords(p->field)` prints the field again. That
  produced two convincing-looking "arrays" that were just the object's own first two fields.
- **Never read a field off a decompilation and act on it before confirming it is populated at
  runtime.** That produced the empty `+0x88` and the bogus AABB. A dump that agrees with the previous
  line is not evidence; check the plausibility of the *request*, not the output.

**Two dead ends, so nobody walks them again:**

- Every Havok class-name string is also referenced from `.text` at `0x00fb0000`–`0x00fc1000`. Those
  look like a static class-registry table and are not: they are `push <name>` type-mismatch report
  thunks. Their *callers* would be the class's methods, but `bhkWorld`'s thunk at `0x00fbcb00` has
  no callers at all, so nothing in FNV does a checked cast to `bhkWorld`. The thunks do sit in an
  ordered `.rdata` array at `0x01010500`, `0x20` bytes apart.
- `FUN_00c85750` is called with no arguments and its result dereferenced, which looks exactly like a
  singleton accessor. It is `TlsGetValue(DAT_01268108)`. It is called from half of BSHavok, so it is
  an easy false positive to re-find.

**Tooling lives in WSL**, `/home/mart/ghidra.sh <Script.java> [args]` plus
`/home/mart/ghidrascripts/`. It runs `analyzeHeadless` against the persistent project
`~/ghidra-proj/fnv` with `-noanalysis -readOnly`, so a script run costs seconds rather than the 743 s
a full analysis does. Three general-purpose scripts are there and are worth reusing rather than
rewriting: `CallersUp` (caller graph from a seed, N levels deep), `XrefTo` (what points at a raw
address — Ghidra does not always create a `Function` at real code addresses, so `CallersUp` cannot
start there), and `DumpWindow` (a memory range as annotated dwords).

Note the `sed` in `ghidra.sh`: Ghidra prefixes *our* output too, so the prefix has to be stripped
before `/^INFO /d`, or the script deletes its own results.

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
- Logging: the plugin uses its own `src/Log.cpp`, writing `VaultCraft.log` to the game root and
  mirroring to `OutputDebugString`. The mod uses its own slf4j logger.
- Comments in this repo are plain and direct, with no filler. Match that.

## Verifying changes

- `gradlew test` is the only automated test suite, and it is pure Java: `SkyRayTest` and
  `TriColliderTest`. There is no C++ test target.
- Everything else is checked against the running game. Use `tools/`: `dump_collision.py`,
  `probe_feet.py`, `tick_monitor.py` and `trace_motion.py` read the live shared mapping
  - `tools/check_link.py` is the phase 0 check: does the mapping exist, and is the game heartbeating.
- `tools/run_game.ps1` starts the game and presses it through to the last save:

  ```bat
  powershell -ExecutionPolicy Bypass -File tools\run_game.ps1
  ```

  Steam's `steam://rungameid/22380` is the wrong door — it starts `FalloutNVLauncher.exe`, a separate
  launcher app that then has to launch the game itself. The script runs `nvse_loader.exe` instead,
  which is xNVSE's own entry point and starts `FalloutNV.exe` directly with NVSE loaded. Steam still
  has to be running: the game checks for a client at startup, and `nvse_steam_loader.dll` only
  satisfies the handshake, not the check.

  Synthetic key input reaches the game through the keyboard state, so the game only sees it while
  its window is genuinely foreground. `AppActivate` from a background process often fails at that,
  silently, and the keys go to whatever does have focus — which is how the first version of this
  script launched the game and then did nothing to it. The script attaches to the foreground thread's
  input queue to force foreground properly, presses ESC a few times to skip the opening load, and
  then **verifies**: it watches `VaultCraft.log` for `save loaded` and retries the keys up to three
  times. "Did the keys land" is otherwise a guess, and it has already been wrong once.

  `-NoKeys` launches without pressing anything; `-NoVerify` presses without the retry loop.

  **Only run the four probes while the game is actually running.** They were ported from
  SkyCraft unchanged and still use Python's `mmap(tagname=...)`, which calls `CreateFileMapping`
  and therefore *creates* the mapping when it is absent. Run one standalone and it manufactures a
  wrongly-sized stub under the real name; if it is still running when the game starts, the game's
  `CreateFileMapping` succeeds against that stub and its `MapViewOfFile` then fails on the 191 MB
  it asked for. `check_link.py` deliberately uses `OpenFileMappingW` instead, which only opens.