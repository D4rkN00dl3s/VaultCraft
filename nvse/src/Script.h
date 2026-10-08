#pragma once

// Access to the game's scripting engine through xNVSE's plugin interface.
//
// This is where the ground comes from. Phase 1 spent a long time trying to read terrain geometry
// out of Havok and failed; the answer was a script call all along:
//
//     Player.GetPos x                      -> -62767.99
//     GetTerrainHeight -62767.99 -15891.25 ->   7004
//
// Nothing is linked to reach any of this - every interface comes from QueryInterface on
// NVSEInterface, which matters because nvse_1_4.dll exports exactly one symbol, StartNVSE.
namespace vaultcraft::script
{
	// The engine's "no terrain at these coordinates" answer. Not an error, and not a height.
	inline constexpr double kNoTerrain = -2048.0;

	// Called from NVSEPlugin_Load. Safe to call once.
	void Init(NVSEInterface* a_nvse);

	// False if scripting is unavailable, in which case the calls below return false.
	bool Ready();

	// Player position in world units: X and Y are the horizontal plane, Z is up.
	bool PlayerPosition(double& a_x, double& a_y, double& a_z);

	// Ground height beneath the given X and Y. False when there is no terrain there - which is
	// different from a height of zero, so it is reported rather than conflated.
	bool TerrainHeight(const double a_x, const double a_y, double& a_outHeight);

	// Player facing, in degrees. Intentionally NOT compiled at Init: see PlayerAngle.
	//
	// `Player.GetAngle X` returns -89 (looking up) to +89 (looking down). `Player.GetAngle Z`
	// returns 0..360 as a bearing: 0 at north, increasing clockwise, so 90 is east. Minecraft's yaw
	// is 0 at south increasing clockwise, hence the 180 degree offset in World.h; its pitch is
	// positive looking down, same as the X angle, so pitch passes through unchanged.
	//
	// Z is a bearing from the game's coordinate system, not from a compass rose: it is only
	// meaningful in the Wasteland exterior worldspace, and the NorthMarker a compass uses is
	// per-cell.
	bool PlayerAngle(float& a_outZAngle, float& a_outXAngle);

	// Logs position and a small ground sweep once, so a fresh build has something to show.
	void SelfTest();
} // namespace vaultcraft::script