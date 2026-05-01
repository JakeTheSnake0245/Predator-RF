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
#   dsd_ncurses_handler.c   — terminal UI input
#   dsd_ncurses_menu.c      — terminal UI menu
#   dsd_ncurses_printer.c   — terminal UI render
#   dsd_rigctl.c            — Hamlib client (won't link)
#   pa_devs.c               — PortAudio device enum
#   pulse_devices.c         — PulseAudio device enum
#   dsd_serial.c            — termios GPS serial
#   dsd_import.c            — file-based import path
#
# Files we KEEP that are gated/stubbed at the source level:
#
#   dsd_main.c              — vendored main()/usage()/cleanupAndExit()/handler()
#                             gated out via #ifndef PREDATOR_BUILD; we still need
#                             initOpts(), initState(), noCarrier(), liveScanner()
#                             from this file to drive the decoder loop.
#   dsd_file.c              — WAV file I/O; never invoked at runtime in the
#                             Predator build (mbe_out_f / wav_out_f stay NULL),
#                             but its sf_*/sndfile calls resolve to the stubs in
#                             predator_dsdfme_stubs.c so it links cleanly.
# ============================================================

set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# --- mbelib (10 .c files, super clean — no external deps) ---
file(GLOB MBE_SRC ${VENDOR_DIR}/mbelib/*.c)
add_library(predator_mbe STATIC ${MBE_SRC})
target_include_directories(predator_mbe PUBLIC ${VENDOR_DIR}/mbelib)
target_compile_options(predator_mbe PRIVATE -w)
set_property(TARGET predator_mbe PROPERTY POSITION_INDEPENDENT_CODE ON)

# --- DSD-FME core sources, with surgical exclusions ---
# We need both .c (vendored DSD-FME core) and .cpp (P25 ECC: check_NID,
# Reed-Solomon 36/20/17 + 24/12/13 + 24/16/9, Hamming 10/6/3, Golay
# 24/6 + 24/12, ezpwd RS28 FACCH). Without the .cpp glob the link step
# fails on a flood of undefined `check_and_fix_*` / `encode_*` symbols.
file(GLOB DSD_ALL_SRC
    ${VENDOR_DIR}/dsd-fme/src/*.c
    ${VENDOR_DIR}/dsd-fme/src/*.cpp)
list(FILTER DSD_ALL_SRC EXCLUDE REGEX
    "/(dsd_ncurses_handler|dsd_ncurses_menu|dsd_ncurses_printer|dsd_rigctl|pa_devs|pulse_devices|dsd_serial|dsd_import)\\.c$")
# Also exclude rtl_sdr_fm.cpp — it's the upstream RTL-SDR direct-FM
# command-line tool (not used; we feed samples via predator_dsd_bridge).
# No kept code calls into its symbols (low_pass / fm_demod / etc.).
list(FILTER DSD_ALL_SRC EXCLUDE REGEX "/rtl_sdr_fm\\.cpp$")

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
