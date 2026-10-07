#include "PCH.h"
#include "Engine.h"
#include "Log.h"

#include <cstring>

namespace vaultcraft::engine
{
namespace
{
	// A candidate is only interesting if its two data pointers land in readable private memory,
	// because BSHavok allocates through its own pools (MEM_PRIVATE). A pointer into an image or
	// into no memory at all means we matched something that is not a live shape.
	bool IsLivePrivate(const std::uintptr_t a_p)
	{
		if (a_p < 0x00010000u) {
			return false;
		}
		MEMORY_BASIC_INFORMATION mbi{};
		if (::VirtualQuery(reinterpret_cast<LPCVOID>(a_p), &mbi, sizeof(mbi)) == 0) {
			return false;
		}
		if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) {
			return false;
		}
		if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
			return false;
		}
		return a_p < reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	}

	bool IsReadable(const std::uintptr_t a_p)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		if (::VirtualQuery(reinterpret_cast<LPCVOID>(a_p), &mbi, sizeof(mbi)) == 0) {
			return false;
		}
		if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
			(mbi.Protect & PAGE_NOACCESS) != 0) {
			return false;
		}
		return a_p < reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	}

	void DumpWords(const char* a_what, const std::uintptr_t a_p)
	{
		if (!IsReadable(a_p)) {
			log::Info("    %s -> %08X (not readable)", a_what, static_cast<unsigned long>(a_p));
			return;
		}
		const auto* words = reinterpret_cast<const unsigned int*>(a_p);
		log::Info("    %s -> %08X  [%08X %08X %08X %08X]", a_what, static_cast<unsigned long>(a_p),
			words[0], words[1], words[2], words[3]);
	}

	// Keeps the log readable. The answer to "how many are there" is one number; the answer to "what
	// does one look like" is the first handful.
	constexpr int kMaxLogged = 6;
	constexpr int kMaxHits = 512;
} // namespace

int ScanForTerrainShapes()
{
	const std::uint32_t needle = static_cast<std::uint32_t>(addr::kBhkNiTriStripsShapeVtable);
	constexpr std::size_t kNeedleBytes = sizeof(needle);

	SYSTEM_INFO si{};
	::GetSystemInfo(&si);
	const std::uintptr_t kLimit =
		reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

	// Skip the low 64 KiB: it is permanently unmapped and holding the null guard region, and there
	// is nothing useful down there.
	std::uintptr_t cursor = 0x00010000u;

	ULONGLONG tick = ::GetTickCount64();
	int hits = 0;
	int scanned = 0;

	while (cursor < kLimit && hits < kMaxHits) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
			break;
		}
		const std::uintptr_t regionStart =
			reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
		const std::uintptr_t regionEnd = regionStart + mbi.RegionSize;
		if (regionEnd <= cursor) {
			break;
		}

		// MEM_IMAGE is skipped deliberately. The vtable itself lives there, so searching it would
		// always match the class we are looking for, and shapes live on the heap regardless.
		const bool usable = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
			(mbi.Protect & PAGE_GUARD) == 0 && (mbi.Protect & PAGE_NOACCESS) == 0;
		if (usable && mbi.RegionSize >= kNeedleBytes) {
			const auto* base = reinterpret_cast<const unsigned char*>(regionStart);
			scanned += static_cast<int>(mbi.RegionSize);
			for (std::size_t i = 0; i + kNeedleBytes <= mbi.RegionSize; ++i) {
				if (std::memcmp(base + i, &needle, kNeedleBytes) != 0) {
					continue;
				}
				const std::uintptr_t obj = regionStart + i;

				// The vtable must start the object, so there has to be room for the whole thing.
				if (obj + addr::kBhkNiTriStripsShapeSize > regionEnd) {
					continue;
				}
				const auto* o = reinterpret_cast<const unsigned int*>(obj);
				const std::uintptr_t p0c = o[addr::kBhkNiTriStripsShapeSize / 4 - 2];
				const std::uintptr_t p10 = o[addr::kBhkNiTriStripsShapeSize / 4 - 1];
				if (!IsLivePrivate(p0c) || !IsLivePrivate(p10)) {
					continue;
				}

				++hits;
				if (hits <= kMaxLogged) {
					log::Info("terrain shape %d @ %08X  words [%08X %08X %08X %08X %08X]", hits,
						static_cast<unsigned long>(obj), o[0], o[1], o[2], o[3], o[4]);
					DumpWords("+0C", p0c);
					DumpWords("+10", p10);
				}
				if (hits == 1) {
					log::Info("  (further matches are counted but not dumped)");
				}
			}
		}

		cursor = regionEnd;
	}

	tick = ::GetTickCount64() - tick;
	log::Info("terrain shape scan: %d candidate(s) in %d MB, %llu ms", hits, scanned / (1024 * 1024),
		static_cast<unsigned long long>(tick));
	return hits;
}
} // namespace vaultcraft::engine