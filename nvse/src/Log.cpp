#include "Log.h"
#include "PCH.h"

namespace vaultcraft::log
{
	namespace
	{
		FILE* g_file = nullptr;

		void Write(const char* a_level, const char* a_fmt, va_list a_args)
		{
			char body[2048];
			std::vsnprintf(body, sizeof(body), a_fmt, a_args);

			SYSTEMTIME now{};
			::GetLocalTime(&now);
			const int stamp = ::GetCurrentThreadId();

			if (!g_file) {
				// Next to the exe, not the working directory: nvse_loader.exe may start the game from
				// somewhere else entirely, and a log in the wrong folder is no use to anyone.
				wchar_t self[MAX_PATH]{};
				const DWORD len = ::GetModuleFileNameW(nullptr, self, MAX_PATH);
				if (len > 0 && len < MAX_PATH) {
					std::wstring path(self, len);
					if (const auto slash = path.find_last_of(L'\\'); slash != std::wstring::npos) {
						path.resize(slash + 1);
						path += L"VaultCraft.log";
						// Plain "w", deliberately. Any ccsb= token in the mode string - whether
						// UTF-8, 65001 or a bare number - is rejected by this UCRT's parameter
						// validation, which raises the invalid-parameter handler and __fastfail's
						// with 0xC0000409 inside ucrtbase.dll. Measured on 10.0.26100: "w" and "wb"
						// work; "w, ccsb=UTF-8" kills the process with no output and no log file,
						// because opening the file is the first thing every log line does.
						_wfopen_s(&g_file, path.c_str(), L"w");
					}
				}
			}

			if (g_file) {
				::fprintf(g_file, "[%02u:%02u:%02u.%03u tid %05x] %-5s %s\n",
					now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, stamp, a_level, body);
				::fflush(g_file);  // a crash is the usual reason to want this file
			}

			char line[2176];
			std::snprintf(line, sizeof(line), "[VaultCraft %s] %s", a_level, body);
			::OutputDebugStringA(line);
		}
	} // namespace

	std::string Narrow(const wchar_t* a_wide)
	{
		if (!a_wide || !*a_wide) {
			return {};
		}
		const int needed = ::WideCharToMultiByte(CP_UTF8, 0, a_wide, -1, nullptr, 0, nullptr, nullptr);
		if (needed <= 1) {
			return {};
		}
		std::string out(static_cast<std::size_t>(needed - 1), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_wide, -1, out.data(), needed, nullptr, nullptr);
		return out;
	}

	void Init()
	{
		Info("VaultCraft 0.1.0 starting");
	}

	void Info(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		Write("INFO", a_fmt, args);
		va_end(args);
	}

	void Error(const char* a_fmt, ...)
	{
		va_list args;
		va_start(args, a_fmt);
		Write("ERROR", a_fmt, args);
		va_end(args);
	}
} // namespace vaultcraft::log