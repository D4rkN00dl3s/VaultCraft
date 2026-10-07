#pragma once

// Include order matters here, so use NVSE's own prefix header rather than hand-rolling one. It is
// the root of their include graph: common/IPrefix.h defines the primitive types (UInt32 and
// friends), pulls in winsock2 before Windows.h, and silences warnings their headers would trip;
// nvse/utility.h and nvse/containers.h supply UnorderedMap and friends, which PluginAPI.h reaches
// through CommandTable.h and GameExtraData.h without including them itself.
//
// The standard library goes after the prefix and before PluginAPI.h. Get that order wrong and the
// parse collapses, which then shows up as a wall of bogus "static assertion failed" errors about
// struct sizes further down rather than as the missing include it really is.
#include <nvse/prefix.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nvse/PluginAPI.h>

// The protocol is one source of truth, shared with the Minecraft mod. Including it here means its
// static_asserts on every struct's size and offset are checked against the 32-bit compiler too,
// which is the layout that actually ships. A mismatch would otherwise only show up as a silent
// misread of shared memory at runtime.
#include "skycraft_protocol.h"

namespace vaultcraft
{
	// The protocol header is shared with the Minecraft mod and was carried over unchanged, so it
	// still declares its namespace as skycraft::proto. Alias it rather than edit a file both halves
	// mirror, so this project can keep its own namespace and write proto:: like SkyCraft did.
	namespace proto = ::skycraft::proto;
} // namespace vaultcraft