# Credits

## SkyCraft

VaultCraft is a port of **[SkyCraft](https://github.com/chasmlol/SkyCraft)** by **chasmlol**, which
does the same thing for Skyrim: play Skyrim as a Minecraft player, with Minecraft's physics,
inventory and HUD, over a shared-memory link between an SKSE plugin and a Minecraft Fabric mod.

SkyCraft is MIT licensed. Its notice is preserved verbatim in
[LICENSE.skycraft](LICENSE.skycraft), and it covers the parts of this repository listed below.

Carried over from SkyCraft, substantially unaltered:

| Path | What |
|---|---|
| `fabric/` | the whole Minecraft Fabric mod, ~8,500 lines, kept verbatim |
| `protocol/skycraft_protocol.h` | the shared-memory byte layout both halves speak |
| `tools/dump_collision.py`, `probe_feet.py`, `tick_monitor.py`, `trace_motion.py` | read-only probes that decode the live shared mapping |
| `fabric/gradle/**`, `gradlew*`, `.gitattributes` | Loom/Gradle wrapper and line-ending rules |

The Fabric mod still uses the `dev.skycraft` Java package and the `skycraft` mod id, deliberately,
so that this repository diffs cleanly against SkyCraft. That is a naming choice, not a claim of
authorship over the code.

Written for this project, and not present in SkyCraft:

| Path | What |
|---|---|
| `nvse/**` | the entire Fallout: New Vegas plugin, including its CMake project |
| `tools/fnv_dump.py` | captures decrypted code out of a running game; Fallout: New Vegas ships its `.text` encrypted on disk |
| `README.md`, `AGENTS.md`, `.gitignore` | written for this project |
| `LICENSE` | this project's licence |

## xNVSE

[`nvse/extern/NVSE`](https://github.com/xNVSE/NVSE) is included as a git submodule, pinned at
`6.4.9`, and is used **only** for its engine headers (`GameObjects.h`, `GameTypes.h`,
`PluginAPI.h`, `Hooks_DirectInput8Create.h`, and so on). No xNVSE source is compiled into the
plugin, and nothing in it is modified. xNVSE carries its own licence; see the submodule.

Note that xNVSE's own terms ask that you link, rather than redistribute, NVSE. That is why it is a
submodule here and why the release packaging must not vendor it.

## Games and libraries

VaultCraft ships neither game. It requires you to own them, and it contains no assets from either:

- **Fallout: New Vegas** — Bethesda Softworks / Obsidian Entertainment. Used as the host game
  through xNVSE.
- **Minecraft: Java Edition** — Mojang Studios. Used as the host process through Fabric.
- **Fabric / Fabric API / Loom** — FabricMC, Apache-2.0.
- **Prism Launcher** — if the release ever bundles a portable copy, as SkyCraft's did; MIT.

VaultCraft is a fan project and is not affiliated with, endorsed by, or sponsored by any of the
above.