#pragma once

// The mapping between Fallout: New Vegas world units and Minecraft blocks.
//
// This is the join between two games that disagree about almost everything, and the one part of
// phase 2 that is reasoned rather than measured. Read the constants below before trusting them.
//
// Both halves of the protocol carry Minecraft coordinates (SkyState::posX says "Skyrim player feet,
// MC coords"), so the conversion lives here and nowhere else. Every other piece of the plugin can
// speak Minecraft and stay ignorant of Fallout.
namespace vaultcraft
{
	// Set when a save is loaded. The origin is not a constant in any file: FNV's world coordinates run
	// to roughly +/-60,000 and differ per save, so hardcoding one would mean a config file per save.
	// Anchoring on the load point instead means the mirror world always begins at (0, 0) and grows as
	// the player explores, which is what collisionEpoch exists to tell Minecraft about.
	//
	// Origin is the FNV position that maps to MC (0, 0, 0).
	inline double g_originX = 0.0;
	inline double g_originY = 0.0;
	inline double g_originZ = 0.0;

	// Which Minecraft mirror world this save maps onto. Minecraft picks its world by this, so two saves
	// loaded in different places must not collide on one. Set together with the origin by SetOrigin
	// rather than separately, because the two are only meaningful as a pair.
	inline std::uint32_t g_worldId = 0;

	// Sets the mapping's anchor point, and the world it belongs to.
	inline void SetOrigin(const double aX, const double aY, const double aZ)
	{
		g_originX = aX;
		g_originY = aY;
		g_originZ = aZ;
		// A cheap mix of the two horizontal coordinates. This is not a hash anyone relies on for
		// anything but equality, so it does not need to be good - only distinct for distinct places.
		g_worldId = static_cast<std::uint32_t>(static_cast<std::int32_t>(aX)) ^
					(static_cast<std::uint32_t>(static_cast<std::int32_t>(aY)) * 0x9E3779B1u);
	}

	// FNV runs on the same Creation engine as Skyrim, where 1 unit is about 1.428 cm. A Minecraft block
	// is 1 m, so 100 / 1.428 gives 70.0 units per block. The protocol header already carried this from
	// SkyCraft as kUnitsPerBlock, so it is used rather than recomputed: one number, one place.
	static_assert(proto::kUnitsPerBlock == 70.0, "protocol scale moved; update this file too");

	// Whether FNV's +Y (north) is Minecraft's -Z (north). This follows from both engines using
	// right-handed coordinates with X east and Z up, but it has NOT been measured - it is the one
	// assumption here that a run will settle. If the player walks the wrong way relative to Minecraft,
	// this is the constant to flip, and it is a constant precisely so that flipping it is a one-liner.
	inline constexpr double kNorthIsNegZ = -1.0;

	// FNV's z-angle is measured clockwise from north (+Y), like Skyrim's. Minecraft's yaw is measured
	// clockwise from south (+Z), which is the opposite direction, hence the half turn. FNV's x-angle is
	// positive looking down, and so is Minecraft's pitch, so pitch passes through unchanged.
	inline constexpr float kYawFromFnvZ = 180.0f;

	// FNV world position -> Minecraft block position.
	inline void ToMinecraft(const double aX, const double aY, const double aZ,
		double&                 oX, double& oY, double& oZ)
	{
		const double s = 1.0 / proto::kUnitsPerBlock;
		oX = (aX - g_originX) * s;
		oZ = (aY - g_originY) * s * kNorthIsNegZ;
		oY = (aZ - g_originZ) * s;  // FNV's Z is up, and so is Minecraft's Y
	}

	// Minecraft block position -> FNV world position. The inverse of the above, which matters because
	// Minecraft is the authority in phase 2: the plugin moves the FNV player to wherever Minecraft's
	// physics decided they should be.
	inline void FromMinecraft(const double aX, const double aY, const double aZ,
		double&                 oX, double& oY, double& oZ)
	{
		const double s = proto::kUnitsPerBlock;
		oX = g_originX + aX * s;
		oY = g_originY + aZ * s * kNorthIsNegZ;
		oZ = g_originZ + aY * s;
	}

	inline float ToMinecraftYaw(const float aZAngle) { return aZAngle + kYawFromFnvZ; }
} // namespace vaultcraft