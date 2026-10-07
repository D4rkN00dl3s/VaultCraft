#pragma once

#include <cstddef>
#include <cstdint>

// Raw addresses for this exact Fallout: New Vegas build, recovered from a live-memory dump and
// confirmed against the Ghidra project at ~/ghidra-proj/fnv. Fallout: New Vegas has no Address
// Library, so these are absolute and must be re-found by hand after any engine-mutating mod. They
// live in one table in Engine.cpp so there is a single place to re-verify.
namespace vaultcraft::addr
{
	// bhkNiTriStripsShape, the shape class FNV builds its static terrain collision from.
	//
	// FUN_00ca6670 is the class factory, registered by FUN_00c68230 alongside every other shape.
	// It allocates 0x14 bytes, stores this vtable pointer at offset 0, and zeroes 0x0C and 0x10 -
	// so the object size and vtable below are read out of the constructor rather than inferred.
	// Offset 0x0C and 0x10 are the two data pointers; what they point at is what this spike is for.
	constexpr std::uintptr_t kBhkNiTriStripsShapeVtable = 0x010C771C;
	constexpr std::size_t kBhkNiTriStripsShapeSize = 0x14;
}

namespace vaultcraft::engine
{
	// Phase 1 spike, throwaway. Locates terrain collision shapes in the live process by searching
	// committed private memory for the vtable above, then logs what the candidates hold. Answers the
	// one question phase 1 exists to answer - can we get terrain triangles out of Havok - without
	// committing to an architecture first.
	//
	// Blocking and not cheap; call it once, on save load. Returns the number of candidates found.
	int ScanForTerrainShapes();
} // namespace vaultcraft::engine