#include "PCH.h"
#include "Collision.h"

#include "Link.h"
#include "Log.h"
#include "Script.h"
#include "World.h"

namespace vaultcraft::collision
{
namespace
{
	// How far the player may drift from the centre of the patch they were last sent before a new
	// one is sampled. Half the patch, so patches overlap as the player walks and there is never a
	// gap to fall through.
	//
	// 24 rather than 48 because of what sampling now costs: at one block per sample the patch is
	// (2 * 24 + 1)^2 = 2,401 GetTerrainHeight calls, each a script recompile. Larger is cheaper in
	// resends and far more expensive each - and this is the number the engine call exists to fix.
	constexpr double kSampleRadius = 24.0;

	// Grid spacing in Minecraft blocks. One sample per block, which is what the consumer needs:
	//
	// Minecraft buckets triangles into 8x8x8 cells (SkyCollision.readTris keys them by one region
	// from the box's min corner), so a cell of geometry has to be built from samples about a block
	// apart. At 4 blocks a cell would get 2x2 samples and the ground would read as a staircase of
	// flat plates. One block per sample means each cell is an honest 8x8 height field.
	//
	// This is what makes the direct engine call necessary rather than merely nicer. At one block
	// a 96x96 patch is 97x97 = 9,409 samples, and every one is a script recompile - tens of
	// thousands of instructions, once per resend. There is no step size that avoids this: coarser
	// loses the geometry, finer costs the same per-sample compile. See Script.h's TerrainHeight.
	constexpr double kGridStep = 1.0;

	// Minecraft's region size. Must match SkyCollision.REGION_SIZE, because that is what the
	// consumer buckets by.
	constexpr double kRegionSize = 8.0;

	// The last patch sent, so we only resend on real movement.
	double g_sentX = 0.0;
	double g_sentZ = 0.0;
	bool   g_sent = false;

	// Bumped when the save changes, so Minecraft drops geometry from the previous world rather
	// than standing on it.
	std::uint32_t g_epoch = 0;
} // namespace

void Tick()
{
	if (!Link::Get().Valid() || !script::Ready() || !script::PositionKnown()) {
		return;
	}
	double x = 0.0, y = 0.0, z = 0.0;
	if (!script::PlayerPosition(x, y, z)) {
		return;
	}
	const double mcX = 0.0, mcZ = 0.0;
	double       blockX = 0.0, blockY = 0.0, blockZ = 0.0;
	ToMinecraft(x, y, z, blockX, blockY, blockZ);
	(void)mcX;
	(void)mcZ;

	// Only resend when the player has left the safe centre of the patch they already have.
	if (g_sent && (std::abs(blockX - g_sentX) < kSampleRadius * 0.5) &&
		(std::abs(blockZ - g_sentZ) < kSampleRadius * 0.5)) {
		return;
	}
	g_sentX = blockX;
	g_sentZ = blockZ;
	g_sent  = true;

	// Sample a grid of heights. Held in a vector rather than sent point by point, because a message
	// has to be a fixed size known before it is written - the ring publishes a length up front.
	const int      half = static_cast<int>(kSampleRadius / kGridStep);
	const int      side = half * 2 + 1;
	std::vector<float> heights(static_cast<std::size_t>(side) * side);
	bool                anyGround = false;

	for (int gz = 0; gz < side; ++gz) {
		for (int gx = 0; gx < side; ++gx) {
			// Grid position in MC blocks, then back to FNV world units for the query - the engine
			// knows nothing about Minecraft's axes, and going the long way round through the same
			// conversion the player position uses keeps the two consistent by construction.
			const double wx = blockX + (gx - half) * kGridStep;
			const double wz = blockZ + (gz - half) * kGridStep;
			double fx = 0.0, fy = 0.0, fz = 0.0;
			FromMinecraft(wx, 0.0, wz, fx, fy, fz);

			double height = 0.0;
			if (script::TerrainHeight(fx, fy, height)) {
				// GetTerrainHeight returns FNV Z, which is up. Convert to MC Y, which is also up.
				double outX = 0.0, outY = 0.0, outZ = 0.0;
				ToMinecraft(fx, fy, height, outX, outY, outZ);
				heights[static_cast<std::size_t>(gz) * side + gx] = static_cast<float>(outY);
				anyGround = true;
			}
			else {
				heights[static_cast<std::size_t>(gz) * side + gx] = std::numeric_limits<float>::quiet_NaN();
			}
		}
	}

	if (!anyGround) {
		// Interior cells have no terrain height, and a save on a loading screen briefly has none
		// either. Sending an empty patch would tell Minecraft the world is solid there.
		log::Info("collision: no terrain around the player; nothing sent");
		return;
	}

	// One kColTris message per Minecraft region.
	//
	// This split is the whole point, and getting it wrong is why the first attempt did nothing.
	// SkyCollision.readTris keys a message's triangles by ONE region, derived from the box's min
	// corner:
	//
	//     regionKey(floorDiv(minX, 8), floorDiv(minY, 8), floorDiv(minZ, 8))
	//
	// So a single 96-block message put every triangle in one 8-block cell, while
	// trianglesNear only ever looks in the cells a query box touches. The player's own cell was
	// empty, so it fell through - with the ring fully consumed and no error logged anywhere, which
	// is why this took a look at the consumer to find.
	//
	// Emitting one message per region matches what the consumer already expects.
	//
	// Sent as kColTris, not kColRegion: these are the smooth triangles Minecraft's own collider
	// walks against, which is where a slope's continuous feel comes from. kColRegion is voxel
	// shapes for solid geometry Minecraft treats as blocks, and terrain is not that.
	const std::uint32_t maxTrisPerRegion = 128;
	std::uint32_t       regionsSent = 0;
	std::uint32_t       totalTris   = 0;

	const int rx0 = static_cast<int>(std::floor(blockX - kSampleRadius)) / 8;
	const int rx1 = static_cast<int>(std::ceil(blockX + kSampleRadius)) / 8;
	const int rz0 = static_cast<int>(std::floor(blockZ - kSampleRadius)) / 8;
	const int rz1 = static_cast<int>(std::ceil(blockZ + kSampleRadius)) / 8;

	for (int rz = rz0; rz <= rz1; ++rz) {
		for (int rx = rx0; rx <= rx1; ++rx) {
			auto* begin = Link::Get().ColBegin(proto::kColTris,
				sizeof(proto::ColRegion) + maxTrisPerRegion * sizeof(proto::ColTri));
			if (!begin) {
				break;  // ring full; the rest of this patch has nowhere to go
			}
			auto* payload = reinterpret_cast<std::uint8_t*>(begin + sizeof(proto::ColMsgHeader));

			proto::ColRegion region{};
			region.minX  = rx * 8;
			region.maxX  = region.minX + 8;
			region.minZ  = rz * 8;
			region.maxZ  = region.minZ + 8;
			region.minY  = -2048;
			region.maxY  = 2048;
			region.epoch = g_epoch;
			auto* tris = reinterpret_cast<proto::ColTri*>(payload + sizeof(region));

			std::uint32_t count = 0;
			// Each 8x8 region's worth of triangles: 7x7 quads of the grid, two triangles each.
			for (int gz = rz * 8; gz < rz * 8 + 7; ++gz) {
				for (int gx = rx * 8; gx < rx * 8 + 7; ++gx) {
					// Grid indices for the four corners of this cell, from the patch's own frame.
					const int lx = static_cast<int>(std::lround((gx - (blockX - half)) / kGridStep));
					const int lz = static_cast<int>(std::lround((gz - (blockZ - half)) / kGridStep));
					if (lx < 0 || lz < 0 || lx >= side - 1 || lz >= side - 1) {
						continue;  // outside what we sampled
					}
					const std::size_t i00 = static_cast<std::size_t>(lz) * side + lx;
					const std::size_t i10 = i00 + 1;
					const std::size_t i01 = i00 + side;
					const std::size_t i11 = i01 + 1;
					// A cell touching a missing sample is skipped rather than sent with an undefined
					// corner: a vertex at an undefined height is a hole the player falls through.
					if (std::isnan(heights[i00]) || std::isnan(heights[i10]) ||
						std::isnan(heights[i01]) || std::isnan(heights[i11])) {
						continue;
					}
					if (count + 2 > maxTrisPerRegion) {
						break;
					}
					const float bx = static_cast<float>(blockX + lx * kGridStep);
					const float bz = static_cast<float>(blockZ + lz * kGridStep);
					const float ex = static_cast<float>(kGridStep);

					// Winding is counter-clockwise seen from above, so the normal points up and the
					// player stands on it rather than falling through.
					const float triA[9] = { bx, heights[i00], bz, bx + ex, heights[i10], bz,
						bx, heights[i01], bz + ex };
					const float triB[9] = { bx + ex, heights[i10], bz, bx + ex, heights[i11], bz + ex,
						bx, heights[i01], bz + ex };
					std::memcpy(&tris[count].v, triA, sizeof(triA));
					tris[count].flags = proto::kTriTerrain;
					++count;
					std::memcpy(&tris[count].v, triB, sizeof(triB));
					tris[count].flags = proto::kTriTerrain;
					++count;
				}
			}

			if (count == 0) {
				continue;  // no triangles for this region; nothing to publish
			}
			region.count = count;
			std::memcpy(payload, &region, sizeof(region));
			Link::Get().ColCommit(begin, proto::kColTris);
			++regionsSent;
			totalTris += count;
		}
	}

	if (regionsSent == 0) {
		return;
	}
	log::Info("collision: sent %u triangles across %u regions from %dx%d samples around %.1f %.1f",
		totalTris, regionsSent, side, side, blockX, blockZ);
}
} // namespace vaultcraft::collision