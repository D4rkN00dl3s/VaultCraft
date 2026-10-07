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
set(NVSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/NVSE/nvse/nvse")

file(GLOB_RECURSE VAULTCRAFT_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)

add_library(VaultCraft SHARED ${VAULTCRAFT_SOURCES})

set_target_properties(VaultCraft PROPERTIES
    OUTPUT_NAME "VaultCraft"
    PREFIX ""                       # VaultCraft.dll, not libVaultCraft.dll
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

target_include_directories(VaultCraft PRIVATE
    src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
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
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    target_compile_options(VaultCraft PRIVATE /W4 /permissive- /Zc:preprocessor /utf-8 /MP /wd4200)
    target_link_options(VaultCraft PRIVATE /DEBUG:FULL)
endif()

# Development convenience: copy the built DLL next to FalloutNV.exe so xNVSE picks it up.
# xNVSE is not a virtual-filesystem mod manager, so there is no MO2 profile to deploy into.
# A failed copy is a warning, not a build error: a running game holds the DLL.
set(VAULTCRAFT_DEPLOY_DIR "" CACHE PATH "Fallout: New Vegas folder to copy the built plugin into")
if(VAULTCRAFT_DEPLOY_DIR AND EXISTS "${VAULTCRAFT_DEPLOY_DIR}")
    add_custom_command(TARGET VaultCraft POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VAULTCRAFT_DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:VaultCraft>" "${VAULTCRAFT_DEPLOY_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_PDB_FILE:VaultCraft>" "${VAULTCRAFT_DEPLOY_DIR}/"
        COMMENT "Copying VaultCraft.dll to ${VAULTCRAFT_DEPLOY_DIR}"
        VERBATIM)
endif()