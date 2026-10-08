#pragma once

#include <cstddef>
#include <cstdint>

// Raw addresses for this exact Fallout: New Vegas build, recovered from a live-memory dump and
// confirmed against the Ghidra project at ~/ghidra-proj/fnv. Fallout: New Vegas has no Address
// Library, so these are absolute and must be re-found by hand after any engine-mutating mod. The
// table lives in Engine.cpp so there is a single place to re-verify.
//
// What each of these turned out to be, since none of it was obvious and most of it cost a run:
//
//   0x010CA24C  bhkMoppBvTreeShape, terrain - 502 per loaded cell, and the real geometry.
//   0x010CA330  Gamebryo asset loader - what bhkMoppBvTreeShape+0x08 points at. The literal string
//               "_FallOut_3\Platforms\" sits immediately after this vtable. NOT the geometry.
//   0x0102E368  POD data container, 0x30 bytes - reached from the loader at +0x14. Three
//               destructors and refcounting, nothing else: there is no interpreter in it.
//   0x010C755C  tri-strips wrapper, 0xB0 bytes - second vtable at +0x10 (multiple inheritance).
//               Its AABB is at +0x60..+0x7C, read out by FUN_00ca37e0. Those offsets belong to
//               this class alone; on any other they yield a float of 136164352.0.
//   0x010C740C  hkPackedNiTriStripsData.
//
// Phase 2 will ray-cast rather than read vertices. Candidate entry point is
// FUN_00d21450(this, in, out, ctx) in the 0x010C755C vtable - the (this, input, output, context)
// shape a Havok rayCast has. Recovering its input/output struct layouts is the next task.
namespace vaultcraft::addr
{
	// One Havok class and the address of its vtable. Object identity in Havok is the vtable: two
	// objects of different classes cannot share one, and every instance starts with its own.
	struct HavokShapeClass
	{
		const char* name;
		std::uintptr_t vtable;
	};

	// All 35 classes whose vtable could be read out of a BSHavok factory. Derived mechanically from
	// FUN_00c68230, which registers each class by pushing its name and its factory; the factory
	// allocates the object and stores the vtable at offset 0 last, after the base-class vtable.
	//
	// Covers the classes FNV's static terrain is actually built from - bhkNiTriStripsShape,
	// bhkPackedNiTriStripsShape and bhkMoppBvTreeShape - as well as the convex shapes used for
	// characters and props. Sorted by vtable, which the runtime lookup relies on.
	extern const HavokShapeClass kHavokShapeClasses[];
	extern const std::size_t kHavokShapeClassCount;

	// Narrowest address range containing every vtable above. The runtime scan range-checks against
	// this before doing any table lookup, which is what keeps a full scan of the process heap to
	// about a second.
	constexpr std::uintptr_t kVtableLo = 0x01010000u;
	constexpr std::uintptr_t kVtableHi = 0x010CA800u;

	// The object a bhkPackedNiTriStripsShape points at from +0x08, and the one that actually holds
	// the geometry. Deliberately not in kHavokShapeClasses: its factory FUN_00ca5870 references no
	// class-name string, so there is no name to recover for it the way the other 35 were, and
	// inventing one would be worse than saying so.
	//
	// Constructor FUN_00ca39f0, factory FUN_00ca5870, size 0xB0. It has a second vtable at +0x10
	// (multiple inheritance), keeps a refcounted source object at +0x84, and caches copies of that
	// source's fields at +0x90..+0xA0 - which are where the vertex and index arrays sit.
	constexpr std::uint32_t kTriStripsDataWrapperVtable = 0x010C755Cu;

	// Addresses of Havok class-name *strings*, used to find the runtime class registry.
	//
	// These classes are not registered the way shapes are. bhkWorld, bhkCharacterProxy,
	// hkpWorld and hkpPhysicsSystem are each referenced from exactly one place in .text - the
	// push-name type-mismatch thunk around 0x00fb0000 - and nothing calls that thunk, so no
	// static route reaches their constructors. They register at runtime instead.
	//
	// So look for the registry rather than the constructor: any live pointer to one of these
	// strings is a class-registry entry, and the vtable sits beside it. That is how to obtain a
	// vtable for a class that cannot be built statically.
	struct NamedClass { const char* name; std::uint32_t strAddr; };
	extern const NamedClass kNamedClasses[];
	extern const std::size_t kNamedClassCount;
}

namespace vaultcraft::engine
{
	// Phase 1 spike, throwaway. Walks committed memory looking for live Havok objects, identifies
	// each by its vtable, and reports what it found. Answers the question phase 1 exists to ask -
	// can we get at terrain geometry - without committing to an architecture first.
	//
	// Blocking and not cheap. Call it a few times after a cell has finished loading, not on save
	// load itself: terrain collision is built *during* cell load, so scanning at that instant finds
	// nothing and looks identical to the class not being used.
	//
	// Returns the total number of objects found across all classes.
	int ScanForHavokObjects();
} // namespace vaultcraft::engine