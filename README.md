# VaultCraft

Play Fallout: New Vegas as a Minecraft player. You move with Minecraft's physics, carry Minecraft's
inventory and HUD, and fight with Minecraft's weapons.

Neither game is rewritten. Minecraft runs its own game logic, and Fallout: New Vegas runs its
world, NPCs, quests and saves. An xNVSE plugin and a Minecraft Fabric mod talk to each other
through shared memory. Minecraft runs hidden in the background, and the game draws everything.

> **Status: scaffolding.** The Minecraft half is ported and building. The game-side plugin is not
> written yet, so nothing is playable. See [AGENTS.md](AGENTS.md) for what has been established.

> Early and experimental. Expect rough edges, and back up your saves. This is a fan project, not
> affiliated with Bethesda, Obsidian or Mojang, and you need to own both games.

## Where it came from

This is a port of [SkyCraft](https://github.com/chasmlol/SkyCraft), which does the same thing for
Skyrim. The Minecraft half carries over almost unchanged: the Fabric mod and the shared-memory
protocol only know that *a* game is on the other end of the mapping.

The plugin half does not carry over. Fallout: New Vegas differs from Skyrim in every dimension
that matters here — it is 32-bit rather than 64-bit, it renders with Direct3D 9 rather than 11,
Havok is linked statically rather than wrapped by an SDK, and there is no Address Library, so every
hook is a raw address. That half is being written from scratch.

## Requirements

**Fallout: New Vegas** with [xNVSE](https://github.com/xNVSE/NVSE/releases) installed. The plugin
DLL goes in the same folder as `FalloutNV.exe`, not in `Data\`.

**Minecraft**: Java Edition, Minecraft 26.3, Fabric Loader 0.19.5+, Fabric API, and JDK 25.

## Building

```bat
cd fabric
gradlew build
cd ..\nvse
cmake --preset default
cmake --build --preset release
```

The plugin is x86. `CMakeLists.txt` refuses to configure for x64, because a 64-bit plugin cannot
load into a 32-bit game.

See [AGENTS.md](AGENTS.md) for the build details that are easy to get wrong — chiefly that the
game's own executable cannot be reverse engineered off disk, and what to do about that.

## Credits

VaultCraft is a port of [SkyCraft](https://github.com/chasmlol/SkyCraft) by **chasmlol**, which
does the same thing for Skyrim. The Minecraft mod and the shared-memory protocol are his work,
carried over under the MIT licence, credited in [CREDITS.md](CREDITS.md), with the licence text
preserved in [LICENSE.skycraft](LICENSE.skycraft). The Fallout: New Vegas plugin, the memory
dumper and everything else here are new work.

## License

[MIT](LICENSE). Fallout: New Vegas and Minecraft are the property of their respective owners; this
project ships neither.