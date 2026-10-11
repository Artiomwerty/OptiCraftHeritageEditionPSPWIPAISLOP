# psp.cmake — Sony PSP (Allegrex MIPS32r2) build branch for OptiCraft.
#
# Included from the top of CMakeLists.txt when cmake/psp_toolchain.cmake (wired in
# by the psp-* presets) is active, or with -DPLATFORM=PSP. Like wii.cmake and
# ps2.cmake it builds its own target and return()s, so none of the desktop
# SDL2/glad/OpenGL configuration runs.
#
# Target hardware: PSP-2000 and later (64 MB RAM; MEMSIZE=1 grants it).
#
# PSP_APP selects what gets built (one EBOOT per configure):
#   BRINGUP    Smoke test: RAM/CPU/float speed/Memory Stick speed -> bringup.log.
#   DATACHECK  Mounts assets.pak, decodes PNGs, draws them with sceGu, and shows the
#              live control mapping. Tests storage + GPU + input on a real console.
#   GAME       The full game. Backend work is in progress; see PSP_PORT.md.
#
# Output: bin/psp/OptiCraft-PSP.zip, to extract at the Memory Stick root. Pass
# -DPSP_ASSET_PAK=<assets.pak> to bundle a (PSP-sized) pak into it; see
# scripts/make_psp_pak.py.

cmake_minimum_required(VERSION 3.21)

include(${CMAKE_SOURCE_DIR}/cmake/SourceSelection.cmake)

set(PSP_APP "BRINGUP" CACHE STRING "Program to build: BRINGUP, DATACHECK or GAME")
set_property(CACHE PSP_APP PROPERTY STRINGS BRINGUP DATACHECK GAME)
set(PSP_ASSET_PAK "" CACHE FILEPATH "Optional assets.pak to bundle into the install zip")
set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")
set_property(CACHE MC_LOG_LEVEL PROPERTY STRINGS 0 1 2 3)

set(PSP_LIBS pspdebug pspdisplay pspge pspctrl psppower psprtc)
set(PSP_TITLE "OptiCraft")

if(PSP_APP STREQUAL "BRINGUP")
    set(PSP_SOURCES "${CMAKE_SOURCE_DIR}/src/psp/tools/PspBringup.cpp")
    set(PSP_TITLE "OptiCraft Bring-up")
    set(PSP_INSTALL_TXT "${CMAKE_SOURCE_DIR}/src/psp/INSTALL.txt")
elseif(PSP_APP STREQUAL "DATACHECK")
    set(PSP_SOURCES
        "${CMAKE_SOURCE_DIR}/src/psp/tools/PspDataCheck.cpp"
        "${CMAKE_SOURCE_DIR}/src/psp/input/PspControls.cpp"
        "${CMAKE_SOURCE_DIR}/src/external/stb_image.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/Log.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage/AssetPak.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage/PakArchive.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage/PakFile.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage/StdioStream.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage/PathUtils.cpp"
        "${CMAKE_SOURCE_DIR}/src/platform/storage/PosixFileSystem.cpp"
    )
    list(APPEND PSP_LIBS pspgu pspgum pspvram)
    set(PSP_TITLE "OptiCraft Data Check")
    set(PSP_INSTALL_TXT "${CMAKE_SOURCE_DIR}/src/psp/INSTALL_DATACHECK.txt")
elseif(PSP_APP STREQUAL "GAME")
    # The PSP game runs the engine's desktop ("PC legacy") code path -- the one built for weak 32-bit
    # machines -- with the SDL/OpenGL layers replaced by PSP ones: a sceGu renderer
    # (src/platform/RenderAPI_GU_PSP.cpp), sceCtrl input and sceAudio sound. So the source
    # list is the common code plus the src/pc legacy render/world files, minus anything that
    # talks to SDL, OpenGL or Direct3D directly.
    mcbeta_collect_platform_sources(PSP_SOURCES pc)
    mcbeta_collect_platform_sources(_psp_tree psp)
    list(APPEND PSP_SOURCES ${_psp_tree})
    list(REMOVE_DUPLICATES PSP_SOURCES)
    mcbeta_exclude_sources(PSP_SOURCES
        "[/\\\\]pc[/\\\\](Main|CrashHandler)\\.cpp$"
        "[/\\\\]pc[/\\\\](win32|audio|external|lwjgl)[/\\\\]"
        "[/\\\\]pc[/\\\\]render[/\\\\](d3d9[/\\\\]|PcOpenGlRenderBackend)"
        "[/\\\\]platform[/\\\\]RenderAPI_PC\\.cpp$"
        "[/\\\\]psp[/\\\\]tools[/\\\\]"
        "[/\\\\]psp[/\\\\]compat[/\\\\]"
        "[/\\\\]java[/\\\\]JavaNetwork\\.cpp$"
        "[/\\\\]platform[/\\\\]audio[/\\\\]SoundManager_PC\\.cpp$")
    mcbeta_exclude_remote_stats_sources(PSP_SOURCES)
    mcbeta_select_platform_backends(PSP_SOURCES PSP GU_PSP PSP)
    list(APPEND PSP_SOURCES
        "${CMAKE_SOURCE_DIR}/src/ps2/audio/Ps2AdpcmStreamDecoder.cpp"
        "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
        "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c")
    set_source_files_properties(
        "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/ioapi.c"
        "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip/unzip.c"
        PROPERTIES COMPILE_DEFINITIONS USE_FILE32API)
    list(APPEND PSP_LIBS pspgu pspgum pspvram pspaudio psputility pspnet_inet pspnet pspuser z)
    set(PSP_GAME_DEFS PC_LEGACY_BUILD=1 NO_NETWORK PLATFORM_BOUNDED_WORLD=1 PLATFORM_CONSOLE_LOW=1)
    set(PSP_TITLE "OptiCraft")
    set(PSP_INSTALL_TXT "${CMAKE_SOURCE_DIR}/src/psp/INSTALL_GAME.txt")
else()
    message(FATAL_ERROR "PSP_APP must be BRINGUP, DATACHECK or GAME (got '${PSP_APP}')")
endif()
message(STATUS "PSP build: ${PSP_APP}")

add_executable(OptiCraft ${PSP_SOURCES})
set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/psp/OptiCraft"
)
target_compile_features(OptiCraft PRIVATE cxx_std_17)
target_compile_options(OptiCraft PRIVATE -Wall -O2 -fno-math-errno -fno-trapping-math)
target_compile_definitions(OptiCraft PRIVATE PSP_PLATFORM MC_LOG_LEVEL=${MC_LOG_LEVEL} ${PSP_GAME_DEFS})
if(PSP_NO_SKY)
    target_compile_definitions(OptiCraft PRIVATE PSP_NO_SKY)
endif()
if(PSP_NO_AUDIO)
    target_compile_definitions(OptiCraft PRIVATE PSP_NO_AUDIO)
endif()
target_include_directories(OptiCraft PRIVATE
    "${CMAKE_SOURCE_DIR}/src/psp/compat"
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/pc"
    "${CMAKE_SOURCE_DIR}/src/psp"
    "${CMAKE_SOURCE_DIR}/src/net/minecraft/src"
    "${CMAKE_SOURCE_DIR}/src/mods"
    "${CMAKE_SOURCE_DIR}/external/stb"
    "${CMAKE_SOURCE_DIR}/external/zlib/contrib/minizip"
)

# Static-library link order: dependents first.
target_link_libraries(OptiCraft PRIVATE ${PSP_LIBS} pspkernel m)

# Keep an unstripped copy of the ELF (create_pbp_file strips it in Release) so a crash
# address from the on-device report can be turned into a function with psp-addr2line.
# Commands run in the order they are added, so this must come before create_pbp_file.
add_custom_command(TARGET OptiCraft POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:OptiCraft>" "${CMAKE_SOURCE_DIR}/bin/psp/OptiCraft.unstripped.elf"
    VERBATIM)
target_link_options(OptiCraft PRIVATE "-Wl,-Map,${CMAKE_SOURCE_DIR}/bin/psp/OptiCraft.map")
if(PSP_APP STREQUAL "GAME")
    target_link_options(OptiCraft PRIVATE "-Wl,--wrap=pthread_create" "-Wl,--wrap=__cxa_throw")
endif()

# MEMSIZE=1 asks the firmware for all RAM (64 MB on PSP-2000+). Without it,
# homebrew gets only ~24 MB even on a 64 MB console.
create_pbp_file(
    TARGET OptiCraft
    TITLE "${PSP_TITLE}"
    VERSION "${PROJECT_VERSION}"
    MEMSIZE 1
    ICON_PATH "${CMAKE_SOURCE_DIR}/assets/psp/ICON0.PNG"
    BACKGROUND_PATH "${CMAKE_SOURCE_DIR}/assets/psp/PIC1.PNG"
    OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/psp/OptiCraft"
)

# Stage the Memory Stick layout and a zip. "psp-zip" is part of the default build.
set(PSP_STAGE "${CMAKE_SOURCE_DIR}/bin/psp")
set(PSP_STAGE_GAME "${PSP_STAGE}/PSP/GAME/OptiCraft")
add_custom_target(psp-zip ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PSP_STAGE_GAME}"
    COMMAND ${CMAKE_COMMAND} -E copy "${PSP_STAGE}/OptiCraft/EBOOT.PBP" "${PSP_STAGE_GAME}/EBOOT.PBP"
    COMMAND ${CMAKE_COMMAND} -E copy "${PSP_INSTALL_TXT}" "${PSP_STAGE}/INSTALL.txt"
    COMMENT "Staging ${PSP_STAGE_GAME}"
    VERBATIM
)
if(PSP_ASSET_PAK)
    if(NOT EXISTS "${PSP_ASSET_PAK}")
        message(FATAL_ERROR "PSP_ASSET_PAK does not exist: ${PSP_ASSET_PAK}")
    endif()
    add_custom_command(TARGET psp-zip POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${PSP_ASSET_PAK}" "${PSP_STAGE_GAME}/assets.pak"
        VERBATIM)
elseif(PSP_APP STREQUAL "DATACHECK")
    message(WARNING "PSP_ASSET_PAK not set: the zip will not contain assets.pak, so "
                    "the data check will report the pak as missing.")
endif()
add_custom_command(TARGET psp-zip POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E chdir "${PSP_STAGE}"
            ${CMAKE_COMMAND} -E tar cf "${PSP_STAGE}/OptiCraft-PSP.zip" --format=zip PSP INSTALL.txt
    VERBATIM)
add_dependencies(psp-zip OptiCraft)
