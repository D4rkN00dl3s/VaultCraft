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

	private:
		HANDLE        mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
	};
} // namespace vaultcraft