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
	constexpr double kSampleRadius = 48.0;

	// Grid spacing in Minecraft blocks. One sample every 4 blocks: fine enough that a slope reads
	// as a slope, coarse enough that a patch is a manageable number of samples.
	//
	// This is the cost knob. Every sample is one GetTerrainHeight call, and that recompiles a
	// script expression, so the count is what decides whether this runs at all. At 4 blocks a
	// 96x96 patch is 25x25 = 625 samples. See the note in Script.h about replacing the script call
	// with the engine's when this becomes too slow.
	constexpr double kGridStep = 4.0;

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

	// Two triangles per grid cell, skipping any cell that touches a missing sample rather than
	// inventing a corner. A NaN corner would put a vertex at an undefined height and the player
	// would fall through the floor.
	//
	// Sent as kColTris: these are the smooth triangles Minecraft's own collider walks against,
	// which is what gives slopes their continuous feel. kColRegion (the voxel shapes) is for solid
	// geometry Minecraft should treat as blocks, and terrain is not that.
	const std::uint32_t maxTris = static_cast<std::uint32_t>((side - 1) * (side - 1) * 2);
	auto*               begin    = Link::Get().ColBegin(proto::kColTris,
		sizeof(proto::ColRegion) + maxTris * sizeof(proto::ColTri));
	if (!begin) {
		return;
	}
	auto* payload = reinterpret_cast<std::uint8_t*>(begin + sizeof(proto::ColMsgHeader));

	proto::ColRegion region{};
	region.minX = static_cast<std::int32_t>(std::floor(blockX - kSampleRadius));
	region.maxX = static_cast<std::int32_t>(std::ceil(blockX + kSampleRadius));
	region.minZ = static_cast<std::int32_t>(std::floor(blockZ - kSampleRadius));
	region.maxZ = static_cast<std::int32_t>(std::ceil(blockZ + kSampleRadius));
	region.minY = -2048;
	region.maxY = 2048;
	region.epoch = g_epoch;
	std::memcpy(payload, &region, sizeof(region));
	auto* tris = reinterpret_cast<proto::ColTri*>(payload + sizeof(region));

	std::uint32_t count = 0;
	for (int gz = 0; gz < side - 1; ++gz) {
		for (int gx = 0; gx < side - 1; ++gx) {
			const std::size_t i00 = static_cast<std::size_t>(gz) * side + gx;
			const std::size_t i10 = i00 + 1;
			const std::size_t i01 = i00 + side;
			const std::size_t i11 = i01 + 1;
			if (std::isnan(heights[i00]) || std::isnan(heights[i10]) || std::isnan(heights[i01]) ||
				std::isnan(heights[i11])) {
				continue;
			}
			const float bx = static_cast<float>(blockX + (gx - half) * kGridStep);
			const float bz = static_cast<float>(blockZ + (gz - half) * kGridStep);
			const float ex = static_cast<float>(kGridStep);

			// Winding is counter-clockwise seen from above, so the triangle's normal points up and
			// the player stands on it rather than falling through.
			const float v00[3] = { bx, heights[i00], bz };
			const float v10[3] = { bx + ex, heights[i10], bz };
			const float v01[3] = { bx, heights[i01], bz + ex };
			const float v11[3] = { bx + ex, heights[i11], bz + ex };

			const float triA[9] = { v00[0], v00[1], v00[2], v10[0], v10[1], v10[2], v01[0], v01[1], v01[2] };
			const float triB[9] = { v10[0], v10[1], v10[2], v11[0], v11[1], v11[2], v01[0], v01[1], v01[2] };
			std::memcpy(&tris[count].v, triA, sizeof(triA));
			tris[count].flags = proto::kTriTerrain;
			++count;
			std::memcpy(&tris[count].v, triB, sizeof(triB));
			tris[count].flags = proto::kTriTerrain;
			++count;
		}
	}

	if (count == 0) {
		return;  // nothing committed, so nothing to publish
	}
	region.count = count;
	std::memcpy(payload, &region, sizeof(region));

	Link::Get().ColCommit(begin, proto::kColTris);
	log::Info("collision: sent %u triangles over %dx%d samples around %.1f %.1f", count, side, side,
		blockX, blockZ);
}
} // namespace vaultcraft::collision