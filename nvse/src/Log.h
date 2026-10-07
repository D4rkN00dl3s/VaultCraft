#pragma once

namespace vaultcraft::log
{
	// Writes one line to VaultCraft.log next to FalloutNV.exe, and mirrors it to the debugger.
	// Safe to call before Init: the first call opens the file, and if that fails the line still
	// reaches the debugger rather than being lost.
	void Init();
	void Info(const char* a_fmt, ...);
	void Error(const char* a_fmt, ...);
} // namespace vaultcraft::log