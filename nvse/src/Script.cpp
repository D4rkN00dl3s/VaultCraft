#include "PCH.h"
#include "Script.h"

#include <string>

#include "Log.h"

// xNVSE's NVSEArrayVarInterface::Element frees string results through this extern variable
// (GameTypes.h:39), and Element's methods are inline in the header, so merely constructing one pulls
// the symbol in. It is defined in NVSE's GameAPI.cpp, which is not exported - nvse_1_4.dll exports
// one symbol, StartNVSE - so it has to be supplied here, at global scope to match the declaration.
//
// NVSE's own release build resolves it to the game's FormHeapFree at 0x00401030 (GameAPI.cpp:159);
// that address is stable because FNV is fixed-base, and it matches FUN_00401030 in the Ghidra
// project. Getting this wrong would corrupt the heap on the first string result, so it is worth a
// comment rather than a shrug.
//
// It must be global. Defining it inside a namespace produces a different symbol and does not
// satisfy the extern, which is a confusing link error rather than an obvious one.
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

namespace vaultcraft::script
{
namespace
{
	NVSEScriptInterface* g_script = nullptr;

	// GetPos takes no numeric arguments, so its compiled form can be reused. GetTerrainHeight has to
	// carry its coordinates in the expression text - supplying them at call time does not compile -
	// so it cannot be cached that way and is recompiled per query. Fine for a one-shot, but phase 2
	// wants this every frame, and the fix is to call g_TES->GetTerrainHeight directly or use the
	// engine's virtual TESObjectREFR::GetPos(), which returns an NiVector3 and needs no script at all.
	Script* g_posX = nullptr;
	Script* g_posY = nullptr;
	Script* g_posZ = nullptr;

	// Compiles one expression. CompileExpression is right for these one-liners; CompileScript wants
	// a block ("Begin Function{ } ... end") and rejects them.
	Script* CompileExpr(const std::string& a_text)
	{
		return g_script ? g_script->CompileExpression(a_text.c_str()) : nullptr;
	}

	// Runs an expression and returns its numeric result. Returns false if it did not compile or did
	// not produce a number.
	bool RunNum(const std::string& a_text, double& a_out)
	{
		Script* s = CompileExpr(a_text);
		if (!s) {
			return false;
		}
		NVSEArrayVarInterface::Element r;
		g_script->CallFunction(s, nullptr, nullptr, &r, 0);
		if (r.GetType() != NVSEArrayVarInterface::Element::kType_Numeric) {
			return false;
		}
		a_out = r.GetNumber();
		return true;
	}

	bool RunCached(Script* a_script, double& a_out)
	{
		if (!a_script) {
			return false;
		}
		NVSEArrayVarInterface::Element r;
		g_script->CallFunction(a_script, nullptr, nullptr, &r, 0);
		if (r.GetType() != NVSEArrayVarInterface::Element::kType_Numeric) {
			return false;
		}
		a_out = r.GetNumber();
		return true;
	}
} // namespace

void Init(NVSEInterface* a_nvse)
{
	g_script = static_cast<NVSEScriptInterface*>(a_nvse->QueryInterface(kInterface_Script));
	if (!g_script) {
		return;
	}
	// Player.GetPos <var> yields the assigned value, so the argument is the destination script
	// variable and the result comes back through the evaluator. The bare form does not compile.
	g_posX = CompileExpr("Player.GetPos x");
	g_posY = CompileExpr("Player.GetPos y");
	g_posZ = CompileExpr("Player.GetPos z");
}

bool Ready()
{
	return g_script != nullptr && g_posX != nullptr;
}

bool PlayerPosition(double& a_x, double& a_y, double& a_z)
{
	if (!Ready()) {
		return false;
	}
	return RunCached(g_posX, a_x) && RunCached(g_posY, a_y) && RunCached(g_posZ, a_z);
}

bool TerrainHeight(const double a_x, const double a_y, double& a_outHeight)
{
	if (!g_script) {
		return false;
	}
	char buf[96];
	std::snprintf(buf, sizeof(buf), "GetTerrainHeight %.2f %.2f", a_x, a_y);
	double h = 0.0;
	if (!RunNum(buf, h)) {
		return false;
	}
	// The engine answers -2048 where there is no terrain. Passing that back as a height would put
	// the player two thousand units underground, so it is reported as absence instead.
	if (h == kNoTerrain) {
		return false;
	}
	a_outHeight = h;
	return true;
}

void SelfTest()
{
	if (!Ready()) {
		log::Error("script: unavailable, so position and ground height are not available");
		return;
	}
	double x = 0.0, y = 0.0, z = 0.0;
	if (!PlayerPosition(x, y, z)) {
		log::Error("script: could not read the player position");
		return;
	}
	log::Info("script: player at %.2f %.2f %.2f", x, y, z);

	double h = 0.0;
	if (TerrainHeight(x, y, h)) {
		log::Info("script: ground at %.2f below the player", z - h);
	}
	else {
		log::Info("script: no ground reported at the player's coordinates");
	}

	// A short sweep, so the log shows the ground actually varying rather than one number that might
	// be a constant. 64 units is roughly a Minecraft block in FNV's scale.
	for (int i = 1; i <= 3; ++i) {
		const double off = 64.0 * i;
		double east = 0.0, north = 0.0;
		if (TerrainHeight(x + off, y, east)) {
			log::Info("script: ground %+.0f east = %.2f", off, east);
		}
		if (TerrainHeight(x, y + off, north)) {
			log::Info("script: ground %+.0f north = %.2f", off, north);
		}
	}
}
} // namespace vaultcraft::script