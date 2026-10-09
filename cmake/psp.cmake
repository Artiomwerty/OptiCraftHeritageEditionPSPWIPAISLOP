# psp.cmake — Sony PSP (Allegrex MIPS32r2) build branch for OptiCraft.
#
# Included from the top of CMakeLists.txt when cmake/psp_toolchain.cmake (wired in
# by the psp-* presets) is active, or with -DPLATFORM=PSP. Like wii.cmake and
# ps2.cmake it builds its own target and return()s, so none of the desktop
# SDL2/glad/OpenGL configuration runs.
#
# Target hardware: PSP-2000 and later (64 MB RAM; MEMSIZE=1 grants it).
#
#   psp-bringup   Smoke test only: prints RAM/CPU/float speed/storage speed and
#                 writes bringup.log. Proves toolchain + EBOOT + console agree.
#   psp-release   The full game. Backend work is in progress; see PSP_PORT.md.
#
# Output: bin/psp/OptiCraft/EBOOT.PBP, the Memory Stick tree bin/psp/PSP/GAME/
# OptiCraft/, and bin/psp/OptiCraft-PSP.zip (extract at the Memory Stick root).

cmake_minimum_required(VERSION 3.21)

include(${CMAKE_SOURCE_DIR}/cmake/SourceSelection.cmake)

option(PSP_BRINGUP "Build only the PSP hardware smoke test instead of the game" ON)
set(MC_LOG_LEVEL "0" CACHE STRING "Unified diagnostic verbosity: 0=off, 1=info, 2=debug, 3=trace")
set_property(CACHE MC_LOG_LEVEL PROPERTY STRINGS 0 1 2 3)

if(PSP_BRINGUP)
    set(PSP_SOURCES "${CMAKE_SOURCE_DIR}/src/psp/tools/PspBringup.cpp")
    message(STATUS "PSP build: BRINGUP (hardware smoke test only)")
else()
    message(FATAL_ERROR "The full PSP game backend is not buildable yet; use the psp-bringup preset.")
endif()

add_executable(OptiCraft ${PSP_SOURCES})
set_target_properties(OptiCraft PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/psp/OptiCraft"
)
target_compile_features(OptiCraft PRIVATE cxx_std_17)
target_compile_options(OptiCraft PRIVATE -Wall -O2 -fno-math-errno -fno-trapping-math)
target_compile_definitions(OptiCraft PRIVATE PSP_PLATFORM MC_LOG_LEVEL=${MC_LOG_LEVEL})
target_include_directories(OptiCraft PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/psp")

# Static-library link order: dependents first.
target_link_libraries(OptiCraft PRIVATE pspdebug pspdisplay pspge pspctrl psppower psprtc)

# MEMSIZE=1 asks the firmware for all RAM (64 MB on PSP-2000+). Without it,
# homebrew gets only ~24 MB even on a 64 MB console.
create_pbp_file(
    TARGET OptiCraft
    TITLE "OptiCraft Bring-up"
    VERSION "${PROJECT_VERSION}"
    MEMSIZE 1
    OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/psp/OptiCraft"
)

# Stage the Memory Stick layout and a zip. "psp-zip" is part of the default build.
set(PSP_STAGE "${CMAKE_SOURCE_DIR}/bin/psp")
add_custom_target(psp-zip ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PSP_STAGE}/PSP/GAME/OptiCraft"
    COMMAND ${CMAKE_COMMAND} -E copy "${PSP_STAGE}/OptiCraft/EBOOT.PBP" "${PSP_STAGE}/PSP/GAME/OptiCraft/EBOOT.PBP"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/src/psp/INSTALL.txt" "${PSP_STAGE}/INSTALL.txt"
    COMMAND ${CMAKE_COMMAND} -E chdir "${PSP_STAGE}"
            ${CMAKE_COMMAND} -E tar cf "${PSP_STAGE}/OptiCraft-PSP.zip" --format=zip PSP INSTALL.txt
    COMMENT "Staging bin/psp/PSP/GAME/OptiCraft and OptiCraft-PSP.zip"
    VERBATIM
)
add_dependencies(psp-zip OptiCraft)
