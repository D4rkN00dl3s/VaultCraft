#pragma once

// Sends the ground to Minecraft, so its physics has something to stand on.
//
// This is what makes the puppet move. Without it Minecraft's player has no floor, its own physics
// decides nothing changes, and the player never moves - so the loop that reads Minecraft back is
// exercised but never asked to do anything. Phase 2 measured exactly that: controls frozen, angle
// published, position pinned at the origin, no error anywhere.
//
// The source is a height field, not triangles: GetTerrainHeight answers "how high is the ground at
// this X and Y", one point at a time. So a patch of ground becomes a grid of samples, and each
// cell of the grid becomes two triangles. That is a coarser approximation than the game's real
// collision - no overhangs, no cliffs undercut, nothing vertical - but it is enough for slopes,
// hills and standing height, which is what Minecraft's movement actually needs.
namespace vaultcraft::collision
{
	// Samples the ground around the player and sends it. Cheap to call every frame: it only
	// re-sends when the player has moved off the centre of the last patch, because terrain does
	// not change and re-sending identical geometry would fill the ring for nothing.
	void Tick();
} // namespace vaultcraft::collision