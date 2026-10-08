#include "PCH.h"
#include "Link.h"
#include "Log.h"
#include "Script.h"
#include "World.h"

static PluginHandle   g_pluginHandle = kPluginHandle_Invalid;
static NVSEInterface* g_nvse = nullptr;

namespace
{
	// True from the moment a save is loaded. Everything published to Minecraft is gated on it, so a
	// player who is still at the main menu does not look to Minecraft like they are standing at the
	// origin of a world.
	bool g_inGame = false;

	// The puppet's last accepted position, in FNV world units. Minecraft's physics is the authority
	// from here on, so this is only used to notice that the player has not moved, and to avoid
	// re-running a script call every single frame for nothing.
	//
	// Deduplication matters more than it looks: SetPosition recompiles a script expression every
	// call, so doing it 60 times a second for a player standing still is pure cost. Only a real
	// change is worth paying for.
	double g_puppetX = 0.0;
	double g_puppetY = 0.0;
	double g_puppetZ = 0.0;
	bool   g_puppetKnown = false;

	// One FNV unit in Minecraft blocks. A nudge smaller than this is noise - MC interpolates
	// between ticks, so it republishes the same position with sub-block wobble forever, and
	// reacting to that would move the player every frame and fight the physics that produced it.
	constexpr double kMinMoveUnits = 0.35;

	// True once Minecraft has taken over movement, so it is only disabled on the way out and the
	// player is not left frozen if Minecraft never connects.
	bool g_movementFrozen = false;

	// Moves the FNV player to wherever Minecraft's physics decided they should be, and stops the
	// game's own controller from arguing about it.
	//
	// Both halves matter. Setting the position alone gets undone by FNV's movement code on the same
	// frame, which reads its own animation and collision and pulls the player back. Freezing its
	// controls alone leaves the player welded wherever they were standing. Together, Minecraft owns
	// the position and FNV renders it.
	void DrivePuppet(const vaultcraft::proto::McState& a_mc)
	{
		if (!(a_mc.flags & vaultcraft::proto::kMcInWorld)) {
			return;  // a menu, a title screen, or a world that has not finished loading
		}
		if (!vaultcraft::script::Ready()) {
			return;
		}
		if (!g_movementFrozen) {
			vaultcraft::script::FreezeMovement(true);
			g_movementFrozen = true;
			vaultcraft::log::Info("puppet: FNV controls frozen, Minecraft owns movement");
		}

		double fx = 0.0, fy = 0.0, fz = 0.0;
		vaultcraft::FromMinecraft(a_mc.x, a_mc.y, a_mc.z, fx, fy, fz);

		if (g_puppetKnown) {
			const double dx = fx - g_puppetX, dy = fy - g_puppetY, dz = fz - g_puppetZ;
			if (dx * dx + dy * dy + dz * dz < kMinMoveUnits * kMinMoveUnits) {
				return;  // standing still; do not pay for another script call
			}
		}
		// Absolute, because Minecraft is authoritative: its position is where the player IS, not a
		// delta to apply on top of wherever FNV currently thinks they are.
		if (vaultcraft::script::SetPosition(fx, fy, fz)) {
			g_puppetX = fx;
			g_puppetY = fy;
			g_puppetZ = fz;
			g_puppetKnown = true;
		}
	}

	// Publishes where the player is, in Minecraft's coordinates, once per frame.
	//
	// Phase 2 has Minecraft as the authority over movement, but the player starts out being driven
	// by the game, so the game has to publish where that leaves them. That is also the calibration
	// step: both halves can then be compared against each other, which is how the one unmeasured
	// constant in World.h - the sign of the north axis - gets settled.
	void PublishState()
	{
		if (!g_inGame || !vaultcraft::script::Ready()) {
			return;
		}
		double x = 0.0, y = 0.0, z = 0.0;
		if (!vaultcraft::script::PlayerPosition(x, y, z)) {
			return;
		}
		double mcX = 0.0, mcY = 0.0, mcZ = 0.0;
		vaultcraft::ToMinecraft(x, y, z, mcX, mcY, mcZ);

		vaultcraft::proto::SkyState state{};
		state.flags   = vaultcraft::proto::kSkyInGame;
		state.worldId = vaultcraft::g_worldId;
		state.posX    = mcX;
		state.posY    = mcY;
		state.posZ    = mcZ;

		// FNV's Z bearing is 0 at north and runs clockwise; Minecraft's yaw is 0 at south and runs
		// clockwise. The offset between them is 180 degrees. X is positive looking down in both.
		float zAngle = 0.0f, xAngle = 0.0f;
		if (vaultcraft::script::PlayerAngle(zAngle, xAngle)) {
			state.yaw   = vaultcraft::ToMinecraftYaw(zAngle);
			state.pitch = xAngle;
		}
		vaultcraft::Link::Get().WriteSkyState(state);
	}

	// The game's main loop is where the heartbeat goes. A dedicated thread would be easier to
	// reason about in isolation, but this is already once per frame and already the pace Minecraft
	// synchronises to, so a second thread would buy nothing here.
	void OnMessage(NVSEMessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case NVSEMessagingInterface::kMessage_MainGameLoop:
			vaultcraft::Link::Get().Heartbeat();
			PublishState();
			// Minecraft's physics wins, so read what it decided and follow. Read after publishing:
			// the game is authoritative for where the player is until Minecraft takes over, and
			// this frame's reading is what drives them next frame.
			if (vaultcraft::proto::McState mc{};
				vaultcraft::Link::Get().ReadMcState(mc)) {
				DrivePuppet(mc);
			}
			break;
		case NVSEMessagingInterface::kMessage_PostLoad:
			vaultcraft::log::Info("game loaded");
			break;
		case NVSEMessagingInterface::kMessage_PostLoadGame: {
			vaultcraft::log::Info("save loaded");
			// Once a cell has finished loading, so the player exists and the ground is queryable.
			// Terrain collision is built *during* cell load, so asking earlier returns nothing.
			vaultcraft::script::SelfTest();
			double ox = 0.0, oy = 0.0, oz = 0.0;
			if (vaultcraft::script::PlayerPosition(ox, oy, oz)) {
				// Anchor the mapping where the player loaded, so the Minecraft mirror world begins
				// at (0, 0) whatever the save's own coordinates happen to be - they run to about
				// +/-60,000 and differ per save, so there is nothing to hardcode.
				vaultcraft::SetOrigin(ox, oy, oz);
				g_inGame = true;
				vaultcraft::log::Info("origin %.2f %.2f %.2f -> MC world %u", ox, oy, oz, vaultcraft::g_worldId);
			}
			else {
				vaultcraft::log::Error("no player position at save load; the origin is not set");
			}
			break;
		}
		case NVSEMessagingInterface::kMessage_ExitGame:
			vaultcraft::log::Info("leaving the game");
			// Give the controls back before the save state can matter. Alt resets on load anyway,
			// but leaving a mod holding movement disabled is the kind of thing that outlives the
			// reason for it.
			vaultcraft::script::FreezeMovement(false);
			g_inGame = false;
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