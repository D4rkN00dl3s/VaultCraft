#include "PCH.h"
#include "Link.h"
#include "Log.h"

static PluginHandle   g_pluginHandle = kPluginHandle_Invalid;
static NVSEInterface* g_nvse = nullptr;

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
			break;
		case NVSEMessagingInterface::kMessage_PostLoad:
			vaultcraft::log::Info("game loaded");
			break;
		case NVSEMessagingInterface::kMessage_PostLoadGame:
			vaultcraft::log::Info("save loaded");
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
	messaging->RegisterListener(g_pluginHandle, "VaultCraft", OnMessage);

	if (!vaultcraft::Link::Get().Create()) {
		// Report success anyway. Failing here makes xNVSE unload us, and an unavailable mapping is
		// not always fatal to the session: a stale one held open by a previous run is exactly what
		// restarting the game clears. Stay loaded and inert rather than vanishing.
		vaultcraft::log::Error("could not create the shared mapping; VaultCraft is inert until the game restarts");
	}

	vaultcraft::log::Info("loaded (pid %lu, shared memory %ls)", ::GetCurrentProcessId(), vaultcraft::proto::kMappingName);
	return true;
}