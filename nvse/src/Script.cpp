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
	bool    g_posTried = false;
	bool    positionKnown_ = false;

	// Player.GetAngle takes an axis *character* (X or Z, uppercase) and returns a value - unlike
	// Player.GetPos, whose argument is a destination variable. Reading them as
	// "Player.GetAngle z" with a lowercase variable made the compiler fault, which took the game
	// down inside NVSEPlugin_Load before it finished.
	//
	// Compiled on first use rather than at Init, so a scripting fault here can never happen on the
	// load path again. g_angTried stops it retrying every frame if the commands turn out to be
	// unavailable; per-frame recompilation is worse than a permanently absent angle.
	Script* g_angZ = nullptr;
	Script* g_angX = nullptr;
	bool    g_angTried = false;

	NVSETogglePlayerControlsInterface* g_controls = nullptr;
	const char* const                  kModName   = "VaultCraft";

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
	// Acquire the interface and nothing else. NVSEPlugin_Load runs before the game has built its
	// script compiler, and calling CompileExpression at this point faults - which xNVSE reports as
	// "fatal error occurred at <nonsense address> while loading plugin", with no usable address and
	// no clue which call did it. Compilation therefore happens on first use, at least one save load
	// later. Phase 1 had this right for the wrong reason: its probe ran from
	// kMessage_PostLoadGame, so it compiled long after this point.
	g_script = static_cast<NVSEScriptInterface*>(a_nvse->QueryInterface(kInterface_Script));
	log::Info("script: NVSEScriptInterface %s", g_script ? "acquired" : "UNAVAILABLE");

	// Safe to fetch an interface here even though nothing may be compiled yet - QueryInterface only
	// hands back a pointer to code that already exists, it does not run any of it.
	g_controls = static_cast<NVSETogglePlayerControlsInterface*>(a_nvse->QueryInterface(kInterface_PlayerControls));
	log::Info("script: TogglePlayerControls %s", g_controls ? "acquired" : "UNAVAILABLE");
}

void FreezeMovement(const bool a_freeze)
{
	if (!g_controls) {
		return;
	}
	// All of movement, looking, jumping and running. Minecraft owns all of it, and leaving any one
	// of them enabled lets the game pull the player back toward its own idea of where they are.
	// Alt because it is not savebaked and resets on load: vanilla DisablePlayerControls writes
	// through, and a stuck flag in a saved game is a bad afternoon.
	constexpr std::uint32_t kAll = 0x1 | 0x2 | 0x40 | 0x200 | 0x800;
	if (a_freeze) {
		g_controls->DisablePlayerControlsAlt(kAll, kModName);
	}
	else {
		g_controls->EnablePlayerControlsAlt(kAll, kModName);
	}
}

bool Ready()
{
	return g_script != nullptr;
}

// Compiles the cached position expressions on first use. g_posTried stops a failure from retrying
// every frame, which would be both wasteful and loud.
static void CompilePositionScripts()
{
	if (g_posTried || !g_script) {
		return;
	}
	g_posTried = true;
	// Player.GetPos <var> yields the assigned value, so the argument is the destination script
	// variable and the result comes back through the evaluator. The bare form does not compile.
	g_posX = CompileExpr("Player.GetPos x");
	g_posY = CompileExpr("Player.GetPos y");
	g_posZ = CompileExpr("Player.GetPos z");
	if (!g_posX || !g_posY || !g_posZ) {
		log::Error("script: Player.GetPos did not compile; position will read as unavailable");
	}
}

bool PlayerPosition(double& a_x, double& a_y, double& a_z)
{
	if (!Ready()) {
		return false;
	}
	CompilePositionScripts();
	if (!g_posX || !g_posY || !g_posZ) {
		return false;
	}
	if (!(RunCached(g_posX, a_x) && RunCached(g_posY, a_y) && RunCached(g_posZ, a_z))) {
		return false;
	}
	positionKnown_ = true;
	return true;
}

bool PositionKnown()
{
	return positionKnown_;
}

bool PlayerAngle(float& a_outZAngle, float& a_outXAngle)
{
	if (!g_script) {
		return false;
	}
	if (!g_angTried) {
		g_angTried = true;
		g_angZ = CompileExpr("Player.GetAngle Z");
		g_angX = CompileExpr("Player.GetAngle X");
		if (!g_angZ || !g_angX) {
			log::Error("script: Player.GetAngle did not compile; facing will read as zero");
		}
	}
	if (!g_angZ || !g_angX) {
		return false;
	}
	double z = 0.0, x = 0.0;
	if (!RunCached(g_angZ, z) || !RunCached(g_angX, x)) {
		return false;
	}
	a_outZAngle = static_cast<float>(z);
	a_outXAngle = static_cast<float>(x);
	return true;
}

bool SetPosition(const double a_x, const double a_y, const double a_z)
{
	if (!g_script) {
		return false;
	}
	// CompileScript, not CompileExpression, because this is three statements rather than one
	// expression - and CompileScript is the one that wants a block. Three axis sets inside a single
	// block cost one compile per move instead of three.
	//
	// Two decimal places is deliberate. FNV units are about 1.4 cm, so 0.01 of a unit is roughly a
	// tenth of a millimetre - finer than the game's own float precision can hold meaningfully, and
	// every extra character is more work for the expression compiler on every single move.
	char buf[192];
	std::snprintf(buf, sizeof(buf),
		"Begin Function\n"
		"Player.SetPos X %.2f\n"
		"Player.SetPos Y %.2f\n"
		"Player.SetPos Z %.2f\n"
		"end",
		a_x, a_y, a_z);
	Script* s = g_script->CompileScript(buf);
	if (!s) {
		return false;
	}
	NVSEArrayVarInterface::Element r;
	g_script->CallFunction(s, nullptr, nullptr, &r, 0);
	return true;
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
	// The engine answers 0 where the coordinates fall outside every loaded cell, per the wiki.
	// Zero is also a perfectly legal height, so it cannot be distinguished from real ground by
	// value - and treating it as absence is the safe direction to be wrong in, because a missing
	// sample drops one triangle rather than putting a vertex at sea level in the middle of a
	// mountain. Interior cells always answer 0 here, since terrain height only exists outdoors.
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