#pragma once

#include "PCH.h"

// Access to the game's scripting engine through xNVSE's plugin interface.
//
// This exists because actor position is a Gamebryo scene-graph question, not a physics one, and
// the scene graph is reachable through scripting without any reverse engineering at all. Phase 1
// spent a long time looking for it inside Havok, which was the wrong place to look.
//
// NVSEScriptInterface compiles script and hands results back to C++. kInterface_Script is obtained
// through the ordinary QueryInterface on NVSEInterface, so no symbols are linked - which matters,
// because nvse_1_4.dll exports exactly one.
namespace vaultcraft::script
{
	// Called from NVSEPlugin_Load, once the interface is available.
	void Init(NVSEInterface* a_nvse);

	// One-shot probe, run a few seconds after a save loads. Compiles a handful of candidate
	// expressions and logs what each returns, then walks whatever object comes back looking for
	// pointers into known Havok classes.
	void Probe();
} // namespace vaultcraft::script