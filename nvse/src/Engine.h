#pragma once

#include <cstddef>
#include <cstdint>

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