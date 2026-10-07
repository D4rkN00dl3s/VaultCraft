#include "Link.h"
#include "PCH.h"
#include "Log.h"

#include <sddl.h>

namespace vaultcraft
{
	namespace
	{
		constexpr std::uint64_t kMcTimeoutMs = 3000;

		// Who may open the shared memory: this Windows user, plus the system and administrators, at
		// normal integrity. Said explicitly because a game run as administrator would otherwise make
		// the mapping administrators-only, and Minecraft - which we start through the desktop and so
		// never elevate - could not open it. It would sit there hidden and never connect. Free with
		// LocalFree.
		PSECURITY_DESCRIPTOR SharedWithThisUser()
		{
			std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
			HANDLE       token = nullptr;
			if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
				DWORD size = 0;
				::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				std::vector<std::uint8_t> buffer(size);
				if (size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
					LPWSTR sid = nullptr;
					if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
						sddl += std::wstring(L"(A;;GA;;;") + sid + L")";
						::LocalFree(sid);
					}
				}
				::CloseHandle(token);
			}
			sddl += L"S:(ML;;NW;;;ME)";
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
				log::Error("shared memory: couldn't build its access rules (%u); using the defaults", ::GetLastError());
				return nullptr;
			}
			return descriptor;
		}
	} // namespace

	Link& Link::Get()
	{
		static Link link;
		return link;
	}

	bool Link::Create()
	{
		if (base_) {
			return true;
		}

		// ~113 MB: 32 MB collision ring, 96 MB of overlay triple buffer, 64 MB render ring.
		const auto size = proto::kMappingBytes;

		PSECURITY_DESCRIPTOR       descriptor = SharedWithThisUser();
		SECURITY_ATTRIBUTES        access{ sizeof(access), descriptor, FALSE };
		mapping_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, descriptor ? &access : nullptr, PAGE_READWRITE,
			static_cast<DWORD>(size >> 32), static_cast<DWORD>(size & 0xFFFFFFFF), proto::kMappingName);
		const DWORD created = ::GetLastError();
		if (descriptor) {
			::LocalFree(descriptor);
		}
		if (!mapping_) {
			log::Error("CreateFileMapping failed (%u)", created);
			return false;
		}
		const bool existed = created == ERROR_ALREADY_EXISTS;

		base_ = static_cast<std::uint8_t*>(::MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
		if (!base_) {
			log::Error("MapViewOfFile failed (%u)", ::GetLastError());
			::CloseHandle(mapping_);
			mapping_ = nullptr;
			return false;
		}

		// A stale mapping can survive if Minecraft still has it open from a previous run, so start
		// from a known state rather than trusting what is there.
		auto* header = reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		std::memset(base_ + proto::kOffSkyState, 0, sizeof(proto::SkyState));
		std::memset(base_ + proto::kOffMcState, 0, sizeof(proto::McState));
		std::memset(base_ + proto::kOffInputRing, 0, proto::kInputRingDataOff);
		std::memset(base_ + proto::kOffCollisionRing, 0, proto::kColRingDataOff);
		header->version = proto::kVersion;
		header->skyrimPid = ::GetCurrentProcessId();
		header->skyrimHeartbeatMs = ::GetTickCount64();
		header->magic = proto::kMagic;

		log::Info("shared memory %ls (%llu MB, %ls)", proto::kMappingName,
			static_cast<unsigned long long>(size >> 20), existed ? "reused" : "created");
		return true;
	}

	bool Link::McAlive() const
	{
		if (!base_) {
			return false;
		}
		auto&       header = *reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		const auto  last = std::atomic_ref<std::uint64_t>(header.mcHeartbeatMs).load(std::memory_order_acquire);
		return last != 0 && ::GetTickCount64() - last < kMcTimeoutMs;
	}

	std::uint32_t Link::McPid() const
	{
		if (!base_) {
			return 0;
		}
		auto& header = *reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		return std::atomic_ref<std::uint32_t>(header.mcPid).load(std::memory_order_acquire);
	}

	void Link::Heartbeat()
	{
		if (!base_) {
			return;
		}
		auto& header = *reinterpret_cast<proto::Header*>(base_ + proto::kOffHeader);
		std::atomic_ref<std::uint64_t>(header.skyrimHeartbeatMs).store(::GetTickCount64(), std::memory_order_release);
	}
} // namespace vaultcraft