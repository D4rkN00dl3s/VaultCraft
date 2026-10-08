#include "PCH.h"
#include "Engine.h"
#include "Link.h"
#include "Log.h"
#include "Script.h"

static PluginHandle   g_pluginHandle = kPluginHandle_Invalid;
static NVSEInterface* g_nvse = nullptr;

// Phase 1 spike, throwaway: scan for Havok objects a few times after a cell has finished loading.
//
// The first attempt fired on kMessage_PostLoadGame and found one stray match, because terrain
// collision is built *during* cell load - the scan ran in the same millisecond as the message and the
// cell did not exist yet. The second attempt delayed it and found the difference. Several passes,
// because a streamed cell keeps building for a while after the save is up.
static int   g_scansLeft = 0;
static DWORD g_nextScan = 0;

namespace
{
	// The game's main loop is where the heartbeat goes. A dedicated thread would be easier to
	// reason about in isolation, but this is already once per frame and already the pace Minecraft
	// synchronises to, so a second thread would buy nothing here.
	void OnMessage(NVSEMessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case NVSEMessagingInterface::kMessage_MainGameLoop:
			vaultcraft::Link::Get().Heartbeat();
			if (g_scansLeft > 0 && ::GetTickCount() >= g_nextScan) {
				--g_scansLeft;
				g_nextScan = ::GetTickCount() + 5000;
				vaultcraft::log::Info("havok scan pass %d", 4 - g_scansLeft);
				vaultcraft::engine::ScanForHavokObjects();
				// The script probe matters more than the scan. Actor position is a scene-graph
				// question and needs no physics at all; this runs on the first pass, by which point
				// a cell has finished loading and the player exists.
				if (4 - g_scansLeft == 1) {
					vaultcraft::script::Probe();
				}
			}
			break;
		case NVSEMessagingInterface::kMessage_PostLoad:
			vaultcraft::log::Info("game loaded");
			break;
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			vaultcraft::log::Info("save loaded");
			// Deliberately not on this message: the cell is still being built here.
			g_scansLeft = 4;
			g_nextScan = ::GetTickCount() + 5000;
			break;
		case NVSEMessagingInterface::kMessage_ExitGame:
			vaultcraft::log::Info("leaving the game");
			break;
		default:
			break;
		}
	}
} // namespace

// xNVSE finds these by GetProcAddress on our module (PluginManager.cpp), so both must really be in
// the export table. extern "C" stops the names being mangled; dllexport is what actually exports
// them, and forgetting it produces a DLL that builds cleanly and then fails to load with no useful
// diagnostic. PluginChecker.cpp also probes for NVSEPlugin_Query to decide what a DLL is.
extern "C" __declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* a_nvse, PluginInfo* a_info)
{
	a_info->infoVersion = PluginInfo::kInfoVersion;
	a_info->name = "VaultCraft";
	a_info->version = 1;

	if (a_nvse->nvseVersion < PACKED_NVSE_VERSION) {
		vaultcraft::log::Error("xNVSE is too old (got %08X, need at least %08X)", a_nvse->nvseVersion, PACKED_NVSE_VERSION);
		return false;
	}

	vaultcraft::log::Info("query: xNVSE %08X, runtime %08X", a_nvse->nvseVersion, a_nvse->runtimeVersion);
	return true;
}

extern "C" __declspec(dllexport) bool NVSEPlugin_Load(NVSEInterface* a_nvse)
{
	vaultcraft::log::Init();

	g_pluginHandle = a_nvse->GetPluginHandle();
	g_nvse = a_nvse;

	auto* messaging = static_cast<NVSEMessagingInterface*>(a_nvse->QueryInterface(kInterface_Messaging));
	if (!messaging) {
		vaultcraft::log::Error("no messaging interface; VaultCraft cannot track the game loop");
		return false;
	}

	// Actor access goes through xNVSE's scripting interface, not through the engine.
	vaultcraft::script::Init(a_nvse);
	// The sender must be "NVSE", not our own name. Dispatch_Message walks s_pluginListeners[sender],
	// and every message NVSE raises is dispatched with sender == 0; LookupHandleFromName maps "NVSE"
	// to handle 0, which is the only slot that ever gets walked. Registering under "VaultCraft"
	// files the listener in our own slot, where nothing dispatches to, so the plugin loads, logs
	// nothing further, and silently never hears from the game again.
	messaging->RegisterListener(g_pluginHandle, "NVSE", OnMessage);

	if (!vaultcraft::Link::Get().Create()) {
		// Report success anyway. Failing here makes xNVSE unload us, and an unavailable mapping is
		// not always fatal to the session: a stale one held open by a previous run is exactly what
		// restarting the game clears. Stay loaded and inert rather than vanishing.
		vaultcraft::log::Error("could not create the shared mapping; VaultCraft is inert until the game restarts");
	}

	vaultcraft::log::Info("loaded (pid %lu, shared memory %s)", ::GetCurrentProcessId(),
		vaultcraft::log::Narrow(vaultcraft::proto::kMappingName).c_str());
	return true;
}