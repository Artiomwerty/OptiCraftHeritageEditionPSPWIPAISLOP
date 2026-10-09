# psp_toolchain.cmake — pspdev (psp-gcc / PSPSDK) cross toolchain for the Sony PSP.
#
# Thin wrapper around the toolchain file that ships with pspdev
# ($PSPDEV/psp/share/pspdev.cmake). The wrapper only exists to find PSPDEV on
# any host (Linux, WSL, macOS, MSYS2) without the user having to export it first.
#
# Resolution order for the SDK root:
#   1. -DPSPDEV=<path> on the CMake command line
#   2. $ENV{PSPDEV}
#   3. the usual per-host defaults (/usr/local/pspdev, ~/pspdev, /opt/pspdev)

# CMake re-includes the toolchain file for every try_compile; keep it idempotent.
if(PSP_TOOLCHAIN_INCLUDED)
    return()
endif()
set(PSP_TOOLCHAIN_INCLUDED YES)

if(NOT PSPDEV AND DEFINED ENV{PSPDEV})
    set(PSPDEV "$ENV{PSPDEV}")
endif()

if(NOT PSPDEV)
    foreach(_candidate "/usr/local/pspdev" "$ENV{HOME}/pspdev" "/opt/pspdev")
        if(EXISTS "${_candidate}/psp/share/pspdev.cmake")
            set(PSPDEV "${_candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT PSPDEV OR NOT EXISTS "${PSPDEV}/psp/share/pspdev.cmake")
    message(FATAL_ERROR
        "PSP toolchain not found. Install pspdev from "
        "https://github.com/pspdev/pspdev/releases and either export PSPDEV "
        "(e.g. export PSPDEV=$HOME/pspdev) or pass -DPSPDEV=<path>.")
endif()

# pspdev.cmake insists on the environment variable, and create_pbp_file()
# (CreatePBP.cmake) reads $ENV{PSPDEV} too, so make sure both agree.
set(ENV{PSPDEV} "${PSPDEV}")
set(ENV{PATH} "${PSPDEV}/bin:$ENV{PATH}")

include("${PSPDEV}/psp/share/pspdev.cmake")

# Project-level marker, the same role NINTENDO_WII / PS2 play for their branches.
set(OPTICRAFT_PSP YES)
