#include "PCH.h"
#include "Script.h"
#include "Engine.h"

#include <string>
#include <cstdio>
#include "Log.h"

// xNVSE's NVSEArrayVarInterface::Element frees string results through this extern variable
// (GameTypes.h:39), and Element's methods are inline in the header, so merely constructing one
// pulls the symbol in. It is defined in NVSE's GameAPI.cpp, which is not exported - nvse_1_4.dll
// exports one symbol, StartNVSE - so it has to be supplied here, at global scope to match the
// declaration.
//
// NVSE's own release build resolves it to the game's FormHeapFree at 0x00401030 (GameAPI.cpp:159);
// that address is stable because FNV is fixed-base, and it matches FUN_00401030 in the Ghidra
// project. Getting this wrong would corrupt the heap on the first string result, so it is worth a
// comment rather than a shrug.
const _FormHeap_Free FormHeap_Free = (_FormHeap_Free)0x00401030;

namespace vaultcraft::script
{
namespace
{
	NVSEScriptInterface* g_script = nullptr;
	NVSEArrayVarInterface* g_array = nullptr;

	const char* TypeName(const UInt8 a_type)
	{
		switch (a_type) {
		case NVSEArrayVarInterface::Element::kType_Numeric: return "number";
		case NVSEArrayVarInterface::Element::kType_Form: return "form";
		case NVSEArrayVarInterface::Element::kType_String: return "string";
		case NVSEArrayVarInterface::Element::kType_Array: return "array";
		default: return "invalid";
		}
	}

	// Compiles an expression, runs it, logs what comes back.
	//
	// CompileExpression, not CompileScript. The latter expects a block - "Begin Function{ } ... end"
	// - so handing it one-liners fails to compile, which is exactly what the first probe run did.
	// Every candidate goes through here even when a failure is expected: knowing a function is
	// absent is as useful as knowing it is present.
	// Two compilation routes, because neither alone worked on the first attempt. CompileExpression is
	// the documented way to evaluate an expression but rejected calls like GetTerrainHeight, while
	// CompileScript rejects anything that is not a block - "Begin Function{ } ... end". A command
	// call may need the block form to parse even though it returns a value, so try both and log
	// which one the running game actually accepts.
	Script* TryCompile(const char* a_text)
	{
		if (!g_script) {
			return nullptr;
		}
		Script* s = g_script->CompileExpression(a_text);
		if (s) {
			return s;
		}
		const std::string block =
			std::string("Begin Function{ } SetResultTo ") + a_text + " end";
		return g_script->CompileScript(block.c_str());
	}

	// Returns nothing and hands the form back through a pointer. Returning the Element by value would
	// emit its copy constructor, which references NVSE's CopyCString - another unexported symbol.
	void RunExpr(const char* a_text, TESObjectREFR* a_callingObj = nullptr, TESForm** a_formOut = nullptr)
	{
		if (a_formOut) {
			*a_formOut = nullptr;
		}
		Script* s = TryCompile(a_text);
		if (!s) {
			log::Info("expr %-30s COMPILE FAILED (both routes)", a_text);
			return;
		}
		NVSEArrayVarInterface::Element result;
		g_script->CallFunction(s, a_callingObj, nullptr, &result, 0);

		TESForm* form = result.GetTESForm();
		if (a_formOut) {
			*a_formOut = form;
		}
		log::Info("expr %-30s -> %-8s num=%g form=%08X id=%06X", a_text, TypeName(result.GetType()),
			result.GetNumber(), static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(form)),
			result.GetFormID());
	}

	// Same, but calling the command with its arguments written inline in the expression text.
	void RunInline(const char* a_text)
	{
		RunExpr(a_text);
	}

	// Runs an expression and hands back the numeric result, or 0.0 if it did not compile or did not
	// return a number.
	double RunNum(const char* a_text)
	{
		Script* s = TryCompile(a_text);
		if (!s) {
			log::Info("expr %-30s COMPILE FAILED", a_text);
			return 0.0;
		}
		NVSEArrayVarInterface::Element r;
		g_script->CallFunction(s, nullptr, nullptr, &r, 0);
		if (r.GetType() != NVSEArrayVarInterface::Element::kType_Numeric) {
			log::Info("expr %-30s -> %s (not a number)", a_text, TypeName(r.GetType()));
			return 0.0;
		}
		log::Info("expr %-30s -> %g", a_text, r.GetNumber());
		return r.GetNumber();
	}

	// Same, with two numeric arguments supplied at call time.
	void RunExpr2(const char* a_text, const double a_x, const double a_y)
	{
		if (!g_script) {
			return;
		}
		Script* s = TryCompile(a_text);
		if (!s) {
			log::Info("expr %-30s COMPILE FAILED (both routes)", a_text);
			return;
		}
		NVSEArrayVarInterface::Element result;
		g_script->CallFunction(s, nullptr, nullptr, &result, 2, a_x, a_y);
		log::Info("expr %-22s(%g, %g) -> %-8s num=%g", a_text, a_x, a_y, TypeName(result.GetType()),
			result.GetNumber());
	}

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

	// Walks the object's leading words and reports any that point at something whose first word is
	// a vtable we recognise. This is the point of the whole probe: an actor must own a Havok
	// character proxy, because that is what holds it up, but we cannot name that class - its vtable
	// is unreachable both statically and at runtime. Identifying it by what it references inverts
	// the problem.
	void FindHavokRefs(const char* a_label, const std::uintptr_t a_obj)
	{
		if (!CanRead(a_obj, 0x100)) {
			log::Info("  %s: %08X not readable", a_label, static_cast<unsigned long>(a_obj));
			return;
		}
		log::Info("  %s: scanning 256 bytes at %08X", a_label, static_cast<unsigned long>(a_obj));
		const auto* w = reinterpret_cast<const std::uint32_t*>(a_obj);
		for (std::size_t i = 0; i < 0x100 / 4; ++i) {
			const std::uint32_t p = w[i];
			if (!CanRead(p, 4)) {
				continue;
			}
			const std::uint32_t vt = *reinterpret_cast<const std::uint32_t*>(p);
			const char* name = nullptr;
			for (std::size_t k = 0; k < addr::kHavokShapeClassCount; ++k) {
				if (static_cast<std::uint32_t>(addr::kHavokShapeClasses[k].vtable) == vt) {
					name = addr::kHavokShapeClasses[k].name;
					break;
				}
			}
			if (!name && vt == addr::kTriStripsDataWrapperVtable) {
				name = "tri-strips wrapper (unnamed)";
			}
			if (name) {
				log::Info("  %s +%02X -> %08X  <- %s", a_label, static_cast<unsigned>(i * 4), p, name);
			}
		}
	}
} // namespace

void Init(NVSEInterface* a_nvse)
{
	g_script = static_cast<NVSEScriptInterface*>(a_nvse->QueryInterface(kInterface_Script));
	g_array = static_cast<NVSEArrayVarInterface*>(a_nvse->QueryInterface(kInterface_ArrayVar));
	log::Info("script: NVSEScriptInterface %s, ArrayVar %s", g_script ? "acquired" : "UNAVAILABLE",
		g_array ? "acquired" : "UNAVAILABLE");
}

namespace
{
	// Pulls a position out of whatever GetPos returned. It comes back as a 3-element array, so this
	// needs the array interface rather than a plain number.
	bool ReadPosition(NVSEArrayVarInterface::Element& a_result, double& a_x, double& a_y, double& a_z)
	{
		if (!g_array || a_result.GetType() != NVSEArrayVarInterface::Element::kType_Array) {
			return false;
		}
		NVSEArrayVarInterface::Array* arr = a_result.GetArray();
		const int n = g_array->GetArraySize(arr);
		log::Info("  GetPos array size = %d", n);
		for (int i = 0; i < n && i < 3; ++i) {
			NVSEArrayVarInterface::Element key(static_cast<double>(i));
			NVSEArrayVarInterface::Element out;
			if (!g_array->GetElement(arr, key, out)) {
				continue;
			}
			if (i == 0) a_x = out.GetNumber();
			if (i == 1) a_y = out.GetNumber();
			if (i == 2) a_z = out.GetNumber();
		}
		return true;
	}
} // namespace

void Probe()
{
	if (!g_script) {
		return;
	}

	// Arithmetic first: if this is not 42 the interface is broken and nothing after it means anything.
	RunExpr("6 * 7");

	// Method-call syntax: "Player.GetPos x", not a bare "GetPos" with the reference as a calling
	// object. The axis argument is mandatory - every bare form failed to compile while this one
	// worked and returned the player's real X.
	//
	// GetGlobalRef is *not* how the player is reached: it returns whatever a global already stores,
	// and a reference has to have been put there first with SetGlobalRef. FNV's Player global holds
	// a number, so there was never a reference in it to fetch.
	const double px = RunNum("Player.GetPos x");
	const double py = RunNum("Player.GetPos y");
	const double pz = RunNum("Player.GetPos z");
	log::Info("script: player at %.2f %.2f %.2f", px, py, pz);

	if (px == 0.0 && py == 0.0) {
		log::Info("script: position read as the origin; the axis forms did not work");
		return;
	}

	// The ground query, at the player's actual coordinates. Earlier attempts returned the -2048
	// "nothing here" sentinel because the coordinates tried were nowhere near the player: this axis
	// runs to roughly +/-60k, not the +/-20k the grid sweep assumed, so all 81 of its points were
	// empty space.
	char buf[96];
	std::snprintf(buf, sizeof(buf), "GetTerrainHeight %.2f %.2f", px, py);
	RunExpr(buf);
	// And a ring around the player, to see whether the ground varies and the query is local.
	for (int i = 1; i <= 4; ++i) {
		const double off = 64.0 * i;
		std::snprintf(buf, sizeof(buf), "GetTerrainHeight %.2f %.2f", px + off, py);
		RunExpr(buf);
		std::snprintf(buf, sizeof(buf), "GetTerrainHeight %.2f %.2f", px, py + off);
		RunExpr(buf);
	}
}

} // namespace vaultcraft::script
