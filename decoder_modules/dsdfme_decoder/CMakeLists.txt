cmake_minimum_required(VERSION 3.13)
project(dsdfme_decoder)

# ============================================================
# Predator RF — Native DSD-FME decoder module.
# Vendors lwvmobile/dsd-fme + szechyjs/mbelib (both GPL-2.0+) for in-APK
# P25 Phase 1+2 and DMR Tier 1/2/3 demodulation, including AMBE+2 voice
# decode via mbelib.
#
# Files we EXCLUDE from the build (they pull in PulseAudio / sndfile /
# ncurses / portaudio / hamlib / direct rtl-sdr — none of which exist on
# Android, and all of which we replace via predator_dsdfme_stubs.c):
#
#   dsd_main.c              — vendored main(), rtl-sdr direct
#   dsd_ncurses_handler.c   — terminal UI input
#   dsd_ncurses_menu.c      — terminal UI menu
#   dsd_ncurses_printer.c   — terminal UI render
#   dsd_rigctl.c            — Hamlib client (won't link)
#   pa_devs.c               — PortAudio device enum
#   pulse_devices.c         — PulseAudio device enum
#   dsd_serial.c            — termios GPS serial
#   dsd_import.c            — file-based import path
#   dsd_file.c              — WAV file I/O (we never read/write WAV)
# ============================================================

set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# --- mbelib (10 .c files, super clean — no external deps) ---
file(GLOB MBE_SRC ${VENDOR_DIR}/mbelib/*.c)
add_library(predator_mbe STATIC ${MBE_SRC})
target_include_directories(predator_mbe PUBLIC ${VENDOR_DIR}/mbelib)
target_compile_options(predator_mbe PRIVATE -w)
set_property(TARGET predator_mbe PROPERTY POSITION_INDEPENDENT_CODE ON)

# --- DSD-FME core sources, with surgical exclusions ---
file(GLOB DSD_ALL_SRC ${VENDOR_DIR}/dsd-fme/src/*.c)
list(FILTER DSD_ALL_SRC EXCLUDE REGEX
    "/(dsd_main|dsd_ncurses_handler|dsd_ncurses_menu|dsd_ncurses_printer|dsd_rigctl|pa_devs|pulse_devices|dsd_serial|dsd_import|dsd_file)\\.c$")

# --- ezpwd Reed-Solomon ---
# NOTE: ezpwd is a header-only C++ library; the vendored directory contains
# extension-less header files (asserter, bch, rs, etc.) included via #include
# "ezpwd/rs". Nothing to compile, but the include path is wired below.

# --- Predator stubs for the excluded layers (PulseAudio / sndfile / RTL-SDR /
#     dsd_main symbols) plus the three ring buffers that bridge SDRPP <-> DSD-FME.
set(PREDATOR_STUBS ${VENDOR_DIR}/dsd-fme/src/predator_dsdfme_stubs.c)

# --- Build ---
# SRC must be defined BEFORE include(SDRPP_MODULE_CMAKE) — that template does
# `add_library(${PROJECT_NAME} SHARED ${SRC})`. We give it the C++ wrapper
# entrypoint here, then attach the (much larger) vendored C surface via
# target_sources after the target exists.
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

include(${SDRPP_MODULE_CMAKE})

target_sources(dsdfme_decoder PRIVATE
    ${DSD_ALL_SRC}
    ${PREDATOR_STUBS}
)

target_include_directories(dsdfme_decoder PRIVATE
    ${VENDOR_DIR}/dsd-fme/include
    ${VENDOR_DIR}/mbelib
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/core/src
)

target_link_libraries(dsdfme_decoder PRIVATE predator_mbe)

# Vendored C is third-party — silence its warnings, define our build flag.
target_compile_definitions(dsdfme_decoder PRIVATE
    PREDATOR_BUILD=1
)
target_compile_options(dsdfme_decoder PRIVATE -w)

# C++ wrapper compiles with normal flags (only main.cpp is C++).
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    PROPERTIES COMPILE_FLAGS "")

if (MSVC)
    target_compile_options(dsdfme_decoder PRIVATE /O2 /Ob2 /std:c++17 /EHsc)
else ()
    target_compile_options(dsdfme_decoder PRIVATE -O3 -std=c++17 -Wno-unused-result)
endif ()
