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

	// Hex alone cannot tell a vertex buffer from an index buffer: both are arrays of integers. The
	// floats are what identify the format, so print both and let the values settle it.
	void DumpAsFloats(const char* a_label, const std::uintptr_t a_p, const int a_count)
	{
		if (!CanRead(a_p, static_cast<std::size_t>(a_count) * sizeof(std::uint32_t))) {
			log::Info("      %s -> %08X (not readable)", a_label, static_cast<unsigned long>(a_p));
			return;
		}
		const auto* w = reinterpret_cast<const std::uint32_t*>(a_p);
		for (int i = 0; i < a_count; ++i) {
			float f = 0.0f;
			std::memcpy(&f, &w[i], sizeof(f));
			log::Info("      %s +%02X = %08X  %12.4f", a_label, i * 4,
				static_cast<unsigned long>(w[i]), static_cast<double>(f));
		}
	}

	// Follows whatever a shape's geometry pointer lands on, whether or not we can name it, and
	// dumps the two arrays. The data class the shape actually points at measured as vtable
	// 0x010C755C, which is not the 0x010C740C the factory extraction produced for
	// hkPackedNiTriStripsData - so naming it first would have blocked the one dump that matters.
	void DumpDataObject(const char* a_label, const std::uintptr_t a_p)
	{
		if (!CanRead(a_p, 0x30)) {
			log::Info("      %s -> %08X (unreadable)", a_label, static_cast<unsigned long>(a_p));
			return;
		}
		const auto* d = reinterpret_cast<const std::uint32_t*>(a_p);
		const int cls = Lookup(d[0]);
		log::Info("      %s -> %08X  vtable %08X%s", a_label, static_cast<unsigned long>(a_p), d[0],
			cls >= 0 ? ("   <- " + std::string(addr::kHavokShapeClasses[cls].name)).c_str() : "");

		if (d[0] == addr::kTriStripsDataWrapperVtable) {
			// The 0xB0-byte wrapper: geometry is in its cached copies at +0x94 and +0x98, and the
			// source object it copied them from is at +0x84. Dumping only the 0x30-byte source's
			// +0x14/+0x18 was the earlier plan and it read the wrapper's middle, which is defaults.
			// The caller's 0x30 check does not cover +0xA0, so check the real extent here.
			if (!CanRead(a_p, 0xB0)) {
				log::Info("      %s wrapper truncated at %08X", a_label, static_cast<unsigned long>(a_p));
				return;
			}
			log::Info("      %s wrapper: src@+84=%08X  count@+A0=%u", a_label, d[0x84 / 4], d[0xA0 / 4]);
			// Triangle layout, from FUN_00ca45b0, the shape's triangle accessor:
			//   this+0x88 -> page table, one 8-byte entry per 2^20 triangles
			//   page      -> block; block+0x20 = vertex array base
			//   block+0x44 = u16 sub-triangle count, block+0x48 = u16 sub-triangle boundaries
			// A vertex is three floats, 12 bytes; indices are u16. The index stream is packed, the
			// positions are not - which is the opposite of what the raw arrays suggested.
			const std::uint32_t pageTable = d[0x88 / 4];
			if (!CanRead(pageTable, 8)) {
				log::Info("      %s page table %08X unreadable", a_label, pageTable);
				return;
			}
			const std::uint32_t block = *reinterpret_cast<const std::uint32_t*>(pageTable);
			if (!CanRead(block, 0x4C)) {
				log::Info("      %s page0 block %08X unreadable", a_label, block);
				return;
			}
			const std::uint32_t vertBase = *reinterpret_cast<const std::uint32_t*>(block + 0x20);
			const std::uint16_t subCount =
				*reinterpret_cast<const std::uint16_t*>(block + 0x44);
			const std::uint32_t subOff = *reinterpret_cast<const std::uint32_t*>(block + 0x48);
			log::Info("      %s page0 block=%08X  verts=%08X  subTris=%u  subOff=%08X", a_label,
				block, vertBase, subCount, subOff);
			// Four vertices = twelve floats. FNV is Y-up with Z south, so a run of plausible
			// magnitudes across three consecutive floats is the confirmation that this is right.
			DumpAsFloats("verts", vertBase, 12);
			return;
		}

		log::Info("      %s counts +08=%u  +0C=%u  +10=%u", a_label, d[2], d[3], d[4]);
		DumpAsFloats("+14", a_p + 0x14, 10);
		DumpAsFloats("+18", a_p + 0x18, 10);
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
	int IndexOfClass(const char* a_name)
	{
		for (std::size_t i = 0; i < addr::kHavokShapeClassCount; ++i) {
			if (std::strcmp(addr::kHavokShapeClasses[i].name, a_name) == 0) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

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
	int populated = 0;

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

				// The geometry pointer on a bhkPackedNiTriStripsShape is at +0x08, not at one of the fields the
				// factory zeroes. Read off a live object: the MOPP shapes embed a tri-strips shape at
				// +0x14, and its +0x04/+0x08/+0x0C/+0x10 line up with the standalone shapes'
				// 1 / pointer / 0 / 0. The constructor only told us which fields start at zero, which
				// is not the same as which one holds the data.
				if (std::strcmp(addr::kHavokShapeClasses[idx].name, "bhkMoppBvTreeShape") == 0) {
					// The 501 tri-strips shapes turned out to be empty shells - wrapper+0x88 is null on
					// all but one, and that one points into unmapped memory. So the geometry lives in
					// the 502 MOPP shapes, and this follows their +0x08, which nothing has read yet.
					const std::uint32_t child = reinterpret_cast<const std::uint32_t*>(obj)[2];
					log::Info("      mopp+08 -> %08X", child);
					if (CanRead(child, 0x30)) {
						DumpWords("child", child, 12);
						// The child carries pointers at +0x14/+0x18 and floats that include a value
						// like 63647.6, which is an FNV world coordinate. That is the same signature
						// the tri-strips data object had, so read both as floats: if these are the
						// vertex and index arrays, the format question is closed.
						DumpAsFloats("child+14", child + 0x14, 10);
						DumpAsFloats("child+18", child + 0x18, 10);
					}
					// Bounds are at +0x60..+0x7C on the 0x010C755C wrapper (from its own accessor,
					// FUN_00ca37e0). Do not read them here: this is a different class, and the same
					// offsets on it produced a float of 136164352.0, which is not a bound.
				} else if (std::strcmp(addr::kHavokShapeClasses[idx].name,
				                        "bhkPackedNiTriStripsShape") == 0) {
					DumpWords("strip", obj, 6);
				} else if (std::strcmp(addr::kHavokShapeClasses[idx].name,
				                        "hkPackedNiTriStripsData") == 0) {
					// Also dump the data objects on their own. Going through a shape depends on the
					// shape's geometry pointer being at +0x08, which was measured rather than known;
					// these are found directly and need no such assumption.
					DumpDataObject("data", obj);
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
	const int triStrip = IndexOfClass("bhkPackedNiTriStripsShape");
	const int mopp = IndexOfClass("bhkMoppBvTreeShape");
	if (triStrip >= 0 && mopp >= 0) {
		log::Info("shape census: %d tri-strips (empty shells), %d MOPP", counts[triStrip],
			counts[mopp]);
	}
	return total;
}
} // namespace vaultcraft::engine