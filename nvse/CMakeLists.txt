cmake_minimum_required(VERSION 3.25)

# The xNVSE plugin. 32-bit Windows, matching Fallout: New Vegas.
project(VaultCraft VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fallout: New Vegas is an x86 binary. A 64-bit plugin cannot load into it, so this is not optional.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "Fallout: New Vegas is a 32-bit game and so is this plugin. "
        "Configure with -A Win32, not -A x64.")
endif()

# FalloutNV.exe is built with the MSVC release CRT. Static so the plugin does not need the
# redistributable alongside it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Gamebryo renders with Direct3D 9, not 11. There is no CommonLib equivalent for Fallout: New
# Vegas, so the engine types come from the NVSE headers in extern/ and everything engine-specific
# goes through src/Port.h.
set(NVSE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/extern/NVSE")
set(NVSE_DIR "${NVSE_ROOT}/nvse/nvse")

file(GLOB_RECURSE VAULTCRAFT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

add_library(VaultCraft SHARED ${VAULTCRAFT_SOURCES})

set_target_properties(VaultCraft PROPERTIES
    OUTPUT_NAME "VaultCraft"
    PREFIX ""                       # VaultCraft.dll, not libVaultCraft.dll
    PDB_NAME "VaultCraft"
    PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

target_include_directories(VaultCraft PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
    # NVSE's own sources spell their includes three different ways, so all three roots are needed:
    #   nvse/PluginAPI.h   -> extern/NVSE/nvse
    #   common/IDebugLog.h -> extern/NVSE
    #   GameObjects.h      -> extern/NVSE/nvse/nvse
    ${NVSE_ROOT}/nvse
    ${NVSE_ROOT}
    ${NVSE_DIR})

target_precompile_headers(VaultCraft PRIVATE src/PCH.h)

# d3d9 for the game's renderer, dinput8 for input, plus the usual Windows pieces.
target_link_libraries(VaultCraft PRIVATE
    d3d9.lib
    dxguid.lib
    dinput8.lib
    ole32.lib
    oleaut32.lib
    user32.lib
    shell32.lib
    advapi32.lib
    winmm.lib)

target_compile_definitions(VaultCraft PRIVATE
    # NOMINMAX must NOT be set here either. xNVSE's headers call bare min()/max(), which only
    # resolve because windows.h defines them as macros. NOMINMAX removes both and the build dies on
    # 'max': identifier not found. xNVSE's own project does not define it either.
    #
    # The consequence for our own code: those macros will expand inside any std::min or std::max
    # call, so write them as (std::min)(a, b) if one is ever needed.
    _CRT_SECURE_NO_WARNINGS
    # WIN32_LEAN_AND_MEAN must NOT be set here. It stops windows.h including rpc.h, which is the
    # only route to rpcndr.h's `typedef unsigned char byte`, and NVSE's headers use a bare `byte`
    # 36 times without declaring it themselves. Without the macro the build dies on unknown type
    # "byte", and the resulting parse errors cascade into dozens of bogus "static assertion failed"
    # failures about struct sizes that have nothing wrong with them. xNVSE's own project does not
    # define it either.
    WIN32
    _WINDOWS
    _USRDLL
    # xNVSE gates its script-function interface on RUNTIME, which it tests both as `#ifdef RUNTIME`
    # and `#if RUNTIME`, so defining it as 1 satisfies both.
    #
    # EDITOR must be left *undefined*, not defined as 0. Their headers also use `#ifdef EDITOR`,
    # which is satisfied by a 0 just as happily as by a 1, so `EDITOR=0` silently compiles the
    # GECK-only EditorData member into the runtime layout and every BaseFormComponent descendant
    # comes out 0x10 bytes too large.
    RUNTIME=1)

if(MSVC)
    target_compile_options(VaultCraft PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /MP /wd4200)
    target_link_options(VaultCraft PRIVATE /DEBUG:FULL)
endif()

# Development convenience: copy the built plugin where xNVSE will actually find it.
#
# The game root is NOT where plugins go. That folder holds xNVSE's own core DLLs, which its loader
# opens by name. PluginManager.cpp scans exactly one directory for plugins and nowhere else:
#     m_pluginDirectory = falloutDirectory + "Data\\NVSE\\Plugins\\";   (PluginManager.cpp:548)
#     for (IDirectoryIterator iter(m_pluginDirectory.c_str(), "*.dll"); ...)  (line 688)
# So VAULTCRAFT_DEPLOY_DIR is the *game folder*, and the Data\NVSE\Plugins part is appended here.
# Pointing at the game root alone produces a DLL that builds, deploys, and is then silently ignored,
# with no log file to explain why.
#
# A failed copy is a warning, not a build error: a running game holds the DLL.
set(VAULTCRAFT_DEPLOY_DIR "" CACHE PATH "Fallout: New Vegas folder (the one holding FalloutNV.exe)")
if(VAULTCRAFT_DEPLOY_DIR AND EXISTS "${VAULTCRAFT_DEPLOY_DIR}")
    set(VAULTCRAFT_PLUGIN_DIR "${VAULTCRAFT_DEPLOY_DIR}/Data/NVSE/Plugins")
    add_custom_command(TARGET VaultCraft POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VAULTCRAFT_PLUGIN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:VaultCraft>" "${VAULTCRAFT_PLUGIN_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/VaultCraft.pdb" "${VAULTCRAFT_PLUGIN_DIR}/"
        COMMENT "Copying VaultCraft.dll to ${VAULTCRAFT_PLUGIN_DIR}"
        VERBATIM)
endif()