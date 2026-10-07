#include "PCH.h"
#include "Engine.h"
#include "Log.h"

#include <cstring>
#include <vector>

namespace vaultcraft::addr
{
	// Sorted by vtable: the runtime lookup below relies on the cheap range check in the scan loop.
	const HavokShapeClass kHavokShapeClasses[] = {
		{ "bhkMouseSpringAction",       0x0101FFCCu },
		{ "bhkRigidBody",               0x010301B4u },
		{ "bhkAabbPhantom",             0x01030864u },
		{ "bhkMultiSphereShape",        0x01066C5Cu },
		{ "bhkTransformShape",          0x01066D44u },
		{ "bhkHingeConstraint",         0x0108FDDCu },
		{ "bhkListShape",               0x010C485Cu },
		{ "bhkStiffSpringConstraint",   0x010C52BCu },
		{ "bhkBlendCollisionObject",    0x010C53DCu },
		{ "bhkLimitedHingeConstraint",  0x010C5CE4u },
		{ "hkPackedNiTriStripsData",    0x010C740Cu },
		{ "bhkPackedNiTriStripsShape",  0x010C761Cu },
		{ "bhkNiTriStripsShape",        0x010C771Cu },
		{ "bhkMalleableConstraint",     0x010C81ACu },
		{ "bhkOrientHingedBodyAction",  0x010C8914u },
		{ "bhkPoseArray",               0x010C93FCu },
		{ "bhkSpringAction",            0x010C955Cu },
		{ "bhkMotorAction",             0x010C9634u },
		{ "bhkDashpotAction",           0x010C970Cu },
		{ "bhkAngularDashpotAction",    0x010C97E4u },
		{ "bhkBreakableConstraint",     0x010C98BCu },
		{ "bhkWheelConstraint",         0x010C9A14u },
		{ "bhkRagdollLimitsConstraint", 0x010C9B0Cu },
		{ "bhkPrismaticConstraint",     0x010C9C84u },
		{ "bhkFixedConstraint",         0x010C9D7Cu },
		{ "bhkHingeLimitsConstraint",   0x010C9F74u },
		{ "bhkBallSocketConstraintChain", 0x010CA06Cu },
		{ "bhkMoppBvTreeShape",         0x010CA24Cu },
		{ "bhkPlaneShape",              0x010CA4DCu },
		{ "bhkExtendedMeshShapeData",   0x010CA6B4u },
		{ "bhkExtendedMeshShape",       0x010CA744u },
		{ "bhkConvexSweepShape",        0x010CAAF4u },
	};
	const std::size_t kHavokShapeClassCount = sizeof(kHavokShapeClasses) / sizeof(kHavokShapeClasses[0]);
} // namespace vaultcraft::addr

namespace vaultcraft::engine
{
namespace
{
	// Linear, not binary: the scan loop's range check already rejects almost every dword, so this
	// table is never walked far and a search tree would be noise.
	int Lookup(const std::uint32_t a_vtable)
	{
		for (std::size_t i = 0; i < addr::kHavokShapeClassCount; ++i) {
			if (static_cast<std::uint32_t>(addr::kHavokShapeClasses[i].vtable) == a_vtable) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	// True only if the whole range is inside one committed, readable, unguarded region. Anything
	// looser and the scan can fault the game on the last page of the heap, which would cost more
	// than the spike is worth.
	bool CanRead(const std::uintptr_t a_p, const std::size_t a_len)
	{
		if (a_p < 0x00010000u) {
			return false;
		}
		MEMORY_BASIC_INFORMATION mbi{};
		if (::VirtualQuery(reinterpret_cast<LPCVOID>(a_p), &mbi, sizeof(mbi)) == 0) {
			return false;
		}
		if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 ||
			(mbi.Protect & PAGE_NOACCESS) != 0) {
			return false;
		}
		const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
		return a_p >= base && (a_p - base) + a_len <= mbi.RegionSize;
	}

	void DumpWords(const char* a_label, const std::uintptr_t a_p, const int a_count)
	{
		if (!CanRead(a_p, static_cast<std::size_t>(a_count) * sizeof(std::uint32_t))) {
			log::Info("      %s -> %08X (not readable)", a_label, static_cast<unsigned long>(a_p));
			return;
		}
		const auto* w = reinterpret_cast<const std::uint32_t*>(a_p);
		for (int i = 0; i < a_count; ++i) {
			const std::uintptr_t v = w[i];
			const int cls = Lookup(static_cast<std::uint32_t>(v));
			log::Info("      %s +%02X = %08X%s", a_label, i * 4, static_cast<unsigned long>(v),
				cls >= 0 ? ("   <- " + std::string(addr::kHavokShapeClasses[cls].name)).c_str() : "");
		}
	}

	// The classes worth dumping, in the order worth dumping them. A global dump budget is useless
	// here: the scan walks memory in address order, so whichever class happens to sit lowest wins
	// all of it and the terrain classes - the entire point - get nothing.
	const char* const kDumpOrder[] = {
		"bhkPackedNiTriStripsShape",
		"hkPackedNiTriStripsData",
		"bhkMoppBvTreeShape",
		"bhkNiTriStripsShape",
	};
	// The classes worth dumping. A global dump budget is useless here: the scan walks memory in
	// address order, so whichever class happens to sit lowest wins all of it and the terrain
	// classes - the entire point of the spike - get nothing.
	bool WorthDumping(const char* a_name)
	{
		for (const char* n : kDumpOrder) {
			if (std::strcmp(a_name, n) == 0) {
				return true;
			}
		}
		return false;
	}

	constexpr int kDumpEach = 3;
	constexpr int kMaxObjectsPerClass = 40000;
} // namespace

int ScanForHavokObjects()
{
	// Local, not static: a previous run of this spiked counters accumulated across passes, so pass 2
	// reported double pass 1 and it read like the world was still streaming in. It was not - the
	// objects were the same ones being counted twice.
	std::vector<int> counts(addr::kHavokShapeClassCount, 0);
	std::vector<int> dumped(addr::kHavokShapeClassCount, 0);

	int total = 0;

	SYSTEM_INFO si{};
	::GetSystemInfo(&si);
	const std::uintptr_t kLimit = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

	// The low 64 KiB is permanently unmapped and holds the null guard region.
	std::uintptr_t cursor = 0x00010000u;

	const ULONGLONG started = ::GetTickCount64();

	while (cursor < kLimit) {
		MEMORY_BASIC_INFORMATION mbi{};
		if (::VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0) {
			break;
		}
		const std::uintptr_t regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
		const std::uintptr_t regionEnd = regionStart + mbi.RegionSize;
		if (regionEnd <= cursor) {
			break;
		}

		// MEM_IMAGE is skipped: the vtables live there, so searching it would match every class we
		// know about, and no Havok object is ever allocated into the module image.
		const bool usable = mbi.State == MEM_COMMIT && mbi.Type != MEM_IMAGE &&
			(mbi.Protect & PAGE_GUARD) == 0 && (mbi.Protect & PAGE_NOACCESS) == 0 &&
			mbi.RegionSize >= sizeof(std::uint32_t);

		if (usable) {
			const auto* words = reinterpret_cast<const std::uint32_t*>(regionStart);
			const std::size_t n = static_cast<std::size_t>(mbi.RegionSize) / sizeof(std::uint32_t);
			for (std::size_t i = 0; i < n; ++i) {
				const std::uint32_t v = words[i];
				// Cheap reject first. Nearly every dword in the process fails both compares, and that
				// is what keeps a full pass over the heap down to a few hundred ms.
				if (v < addr::kVtableLo || v > addr::kVtableHi) {
					continue;
				}
				const int idx = Lookup(v);
				if (idx < 0 || counts[idx] >= kMaxObjectsPerClass) {
					continue;
				}
				++counts[idx];
				++total;

				const std::uintptr_t obj = regionStart + i * sizeof(std::uint32_t);
				if (dumped[idx] >= kDumpEach || !WorthDumping(addr::kHavokShapeClasses[idx].name)) {
					continue;
				}
				++dumped[idx];

				log::Info("%s @ %08X", addr::kHavokShapeClasses[idx].name,
					static_cast<unsigned long>(obj));
				DumpWords("self", obj, 12);

				// A bhkShape is 0x14 bytes whose last two fields are the geometry pointers. Following
				// them is the whole question: if either lands on an hkPackedNiTriStripsData then the
				// triangle data is reachable, and there is no need to reach the physics world at all.
				if (std::strcmp(addr::kHavokShapeClasses[idx].name, "bhkPackedNiTriStripsShape") == 0) {
					DumpWords("+0C", obj + 0x0C, 1);
					DumpWords("+10", obj + 0x10, 1);
				}
			}
		}

		cursor = regionEnd;
	}

	const ULONGLONG elapsed = ::GetTickCount64() - started;
	int classes = 0;
	for (std::size_t i = 0; i < addr::kHavokShapeClassCount; ++i) {
		if (counts[i] != 0) {
			++classes;
			log::Info("  %-30s %d", addr::kHavokShapeClasses[i].name, counts[i]);
		}
	}
	log::Info("havok scan: %d object(s) across %d class(es), %llu ms", total, classes,
		static_cast<unsigned long long>(elapsed));
	return total;
}
} // namespace vaultcraft::engine