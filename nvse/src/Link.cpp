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

		// 191 MB: 32 MB collision ring, 96 MB of overlay triple buffer, 64 MB render ring.
		// CreateFileMapping takes the size as two DWORDs, so the high half is not optional on a
		// 32-bit build even though this particular total fits comfortably in the low half.
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

		// The logger is narrow-only, on purpose. Passing a wchar_t* to a %s, or a char* to a %ls, makes the
// CRT's printf argument validation fire: it raises a fast-fail (0xc0000409) inside ucrtbase.dll and
// takes the whole game down with it. Narrow wide strings at the call site instead.
log::Info("shared memory %s (%llu MB, %s)", log::Narrow(proto::kMappingName).c_str(),
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

	void Link::WriteSkyState(const proto::SkyState& a_state)
	{
		if (!base_) {
			return;
		}
		auto*       dst  = reinterpret_cast<proto::SkyState*>(base_ + proto::kOffSkyState);
		std::atomic_ref<std::uint32_t> seq(dst->seq);

		// Odd means "a write is in flight". The release fences matter: the reader is in another
		// process, so the odd seq and the fields after it must not be reordered against each other,
		// or Minecraft can read a half-updated position and see it as a settled one.
		const std::uint32_t was = seq.load(std::memory_order_relaxed);
		seq.store(was + 1, std::memory_order_release);
		std::atomic_thread_fence(std::memory_order_release);

		dst->flags          = a_state.flags;
		dst->worldId        = a_state.worldId;
		dst->collisionEpoch = a_state.collisionEpoch;
		dst->posX           = a_state.posX;
		dst->posY           = a_state.posY;
		dst->posZ           = a_state.posZ;
		dst->yaw            = a_state.yaw;
		dst->pitch          = a_state.pitch;
		dst->teleportSeq    = a_state.teleportSeq;
		dst->viewportW      = a_state.viewportW;
		dst->viewportH      = a_state.viewportH;
		dst->gameHour       = a_state.gameHour;

		// Even again, released, so the fields above are visible before the reader can stop spinning.
		seq.store(was + 2, std::memory_order_release);
	}

	bool Link::ReadMcState(proto::McState& a_out)
	{
		if (!base_) {
			return false;
		}
		// 16 is generous: the writer updates this once per frame, so a retry almost always
		// succeeds immediately, and giving up is better than stalling the game loop.
		for (int attempt = 0; attempt < 16; ++attempt) {
			const auto* src      = reinterpret_cast<const proto::McState*>(base_ + proto::kOffMcState);
			const auto  before   = std::atomic_ref<const std::uint32_t>(src->seq).load(std::memory_order_acquire);
			if (before & 1) {
				continue;  // mid-write; the writer will settle it
			}
			std::memcpy(&a_out, src, sizeof(a_out));
			const auto after = std::atomic_ref<const std::uint32_t>(src->seq).load(std::memory_order_acquire);
			if (before == after && !(after & 1)) {
				if (after != 0) {
					// Remember that Minecraft exists at all, independently of its heartbeat, so a
					// reader can distinguish "never ran" from "ran and stopped".
					mcEverSeen_ = true;
				}
				return true;
			}
		}
		return false;
	}

	bool Link::McEverSeen() const
	{
		return mcEverSeen_;
	}

	namespace
	{
		// Messages start 8-byte aligned, and so does each one's total size (8 header + payload), or
		// the reader's `tail += align8(8 + payloadBytes)` would drift out of step with ours.
		constexpr std::uint64_t Align8(const std::uint64_t a_n)
		{
			return (a_n + 7) & ~std::uint64_t(7);
		}
	} // namespace

	std::uint8_t* Link::ColBegin(const std::uint32_t a_type, const std::uint32_t a_payloadBytes)
	{
		if (!base_) {
			return nullptr;
		}
		const std::uint64_t total = Align8(sizeof(proto::ColMsgHeader) + a_payloadBytes);
		// A message of slack beyond the largest one, so a consumer that is exactly a message behind
		// still finds room rather than the writer deciding it is full while the ring is provably
		// empty. Without it the two sides can deadlock on a full ring neither can drain.
		if (total + colMaxMessage_ > proto::kColRingDataBytes) {
			if (!colFullLogged_) {
				colFullLogged_ = true;
				log::Error("collision ring: message of %llu bytes does not fit", (unsigned long long)total);
			}
			return nullptr;
		}
		colMaxMessage_ = (std::max)(colMaxMessage_, total);

		const std::uint64_t pos = colHead_ % proto::kColRingDataBytes;
		// A message that would wrap is split by a pad, telling the consumer to jump to the start of
		// the ring. Without it the tail runs off the end and reads whatever happens to be there.
		if (pos + total > proto::kColRingDataBytes) {
			auto* pad              = reinterpret_cast<proto::ColMsgHeader*>(base_ + proto::kOffCollisionRing +
															proto::kColRingDataOff + pos);
			pad->type              = proto::kColPad;
			pad->payloadBytes      = 0;
			const std::uint64_t after = colHead_ + (proto::kColRingDataBytes - pos);
			// Released after the pad is written: the reader decides to jump from head alone, so
			// head must not become visible first.
			std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(
				base_ + proto::kOffCollisionRing + proto::kColRingHeadOff))
				.store(after, std::memory_order_release);
			colHead_ = after;
			return ColBegin(a_type, a_payloadBytes);  // now unwrapped, or genuinely too big
		}

		// Returned with the size recorded so ColCommit can advance head by the right amount without
		// the caller having to remember it. Pending until then.
		colPending_     = total;
		colPendingType_ = a_type;
		return base_ + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
	}

	void Link::ColCommit(std::uint8_t* a_begin, const std::uint32_t a_type)
	{
		if (!base_ || !a_begin || colPending_ == 0) {
			return;
		}
		auto* hdr             = reinterpret_cast<proto::ColMsgHeader*>(a_begin);
		hdr->type             = a_type;
		hdr->payloadBytes     = static_cast<std::uint32_t>(colPending_ - sizeof(proto::ColMsgHeader));
		// Release, and only now: the payload and the header are both written, so publishing head
		// is what makes the message visible. The reader uses head as its "how much is ready" bound,
		// so advancing it earlier would let it read a message whose header has not landed.
		const std::uint64_t after = colHead_ + colPending_;
		std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(
			base_ + proto::kOffCollisionRing + proto::kColRingHeadOff))
			.store(after, std::memory_order_release);
		colHead_     = after;
		colPending_  = 0;
	}

	void Link::ColClear(const std::uint32_t a_epoch)
	{
		// The epoch is the payload of the whole message, so it has to be written before the header
		// is finalised - which is why this cannot go through the plain ColBegin/ColCommit pair.
		if (auto* begin = ColBegin(proto::kColClear, sizeof(std::uint32_t))) {
			*reinterpret_cast<std::uint32_t*>(begin + sizeof(proto::ColMsgHeader)) = a_epoch;
			ColCommit(begin, proto::kColClear);
		}
	}
} // namespace vaultcraft