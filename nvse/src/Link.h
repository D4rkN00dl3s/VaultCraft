#pragma once

#include "skycraft_protocol.h"

namespace vaultcraft
{
	// Owner of the shared-memory mapping. The game creates it; Minecraft opens it.
	//
	// Phase 0 covers only creating the mapping and the two heartbeats, which is all that is
	// needed to prove the plugin loads and Minecraft can find it. The collision ring, actor table,
	// render ring and overlay swap arrive with the phases that use them.
	class Link
	{
	public:
		static Link& Get();

		bool Create();
		[[nodiscard]] bool Valid() const { return base_ != nullptr; }

		// True if Minecraft has touched its heartbeat recently.
		[[nodiscard]] bool McAlive() const;
		// Process id Minecraft wrote when it opened the mapping; changes when Minecraft restarts.
		[[nodiscard]] std::uint32_t McPid() const;
		void                      Heartbeat();

		// Publishes the game side of the protocol. a_state is copied in as given except for seq,
		// which this maintains: it is made odd while the copy is in flight and even once settled,
		// which is how Minecraft's reader knows the fields it read belong to one write
		// (SkyLink.readSkyState spins while seq is odd, and re-reads if it changed under it).
		// Call from the game loop, so there is only ever one writer.
		void WriteSkyState(const proto::SkyState& a_state);

		// Reads what Minecraft's physics decided, for the frames since the last call. False when
		// there is nothing new or no mapping, which is the normal idle case rather than a fault.
		//
		// The same seqlock in reverse: seq is sampled, the fields copied, seq sampled again, and
		// the copy rejected unless both samples agree and were even. Minecraft writes this from its
		// render thread while we read from the game loop, so a torn read is a real possibility and
		// would show up as the player teleporting across the map.
		[[nodiscard]] bool ReadMcState(proto::McState& a_out);

		// True if Minecraft has ever written to the link. Stays true once it has, so a caller can
		// tell "never connected" from "connected and then went away".
		[[nodiscard]] bool McEverSeen() const;

		// Collision ring: the world's ground as Minecraft sees it. A ring rather than a plain
		// array because the two halves never stop - Minecraft drains it on its own thread while
		// this writes from the game loop, and neither may block waiting for the other.
		//
		// ColBegin reserves space and returns where to write the payload, or nullptr if the ring
		// has no room (the consumer has stopped draining). ColCommit publishes it. Doing it in two
		// steps means a message is either wholly visible or not written at all, which a
		// write-head-first scheme would not guarantee.
		std::uint8_t* ColBegin(std::uint32_t a_type, std::uint32_t a_payloadBytes);
		void          ColCommit(std::uint8_t* a_begin, std::uint32_t a_type);
		// Tells Minecraft to drop everything it has, because the world changed.
		void          ColClear(std::uint32_t a_epoch);

	private:
		HANDLE        mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		bool          mcEverSeen_{ false };
		std::uint64_t colHead_{ 0 };      // total bytes we have ever published
		std::uint64_t colPending_{ 0 };    // size of a reserved-but-uncommitted message, 0 if none
		std::uint32_t colPendingType_{ 0 };
		std::uint64_t colMaxMessage_{ 0 }; // largest message seen, kept for the slack reserve
		bool          colFullLogged_{ false };
	};
} // namespace vaultcraft