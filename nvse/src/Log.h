#pragma once

#include <string>

namespace vaultcraft::log
{
	// Converts a wide string to UTF-8 for logging. The logger is narrow-only on purpose; see the
	// note in Log.cpp for why mixing %ls and %s in one format string is a crash, not a warning.
	std::string Narrow(const wchar_t* a_wide);

	// Writes one line to VaultCraft.log next to FalloutNV.exe, and mirrors it to the debugger.
	// Safe to call before Init: the first call opens the file, and if that fails the line still
	// reaches the debugger rather than being lost.
	void Init();
	void Info(const char* a_fmt, ...);
	void Error(const char* a_fmt, ...);
} // namespace vaultcraft::log