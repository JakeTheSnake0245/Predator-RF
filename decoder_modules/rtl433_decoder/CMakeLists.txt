cmake_minimum_required(VERSION 3.13)
project(rtl433_decoder)

# -----------------------------------------------------------------------------
# Predator RF: native rtl_433 decoder module
#
#   Vendored upstream: rtl_433 24.10  (https://github.com/merbanan/rtl_433)
#   Vendor license:    GPL-2.0-or-later (compatible with SDR++ GPL-3.0)
#
#   We compile rtl_433 as a static C library inside this module. The SDR side
#   (sdr.c, rtl_433.c main) and the network outputs (mongoose, http_server,
#   output_mqtt/influx/rtltcp/trigger/udp) are NOT vendored — Predator feeds
#   samples directly from the SDRPP DSP graph and routes decoded events into
#   its own DecoderIngestEvent queue. Stubs in src/predator_stubs.c keep the
#   linker happy for symbols still referenced by the retained sources.
# -----------------------------------------------------------------------------

file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

include(${SDRPP_MODULE_CMAKE})

set(RTL433_VENDOR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/rtl_433)

file(GLOB RTL433_CORE_SRC    "${RTL433_VENDOR}/src/*.c")
file(GLOB RTL433_DEVICES_SRC "${RTL433_VENDOR}/src/devices/*.c")

target_sources(rtl433_decoder PRIVATE
    ${RTL433_CORE_SRC}
    ${RTL433_DEVICES_SRC}
)

target_include_directories(rtl433_decoder PRIVATE
    "src/"
    "${RTL433_VENDOR}/include"
)

# rtl_433 is C99/C11 with lots of warning-laden upstream patterns. Don't fail
# our build on those — they're vendored as-is.
if (NOT MSVC)
    set_source_files_properties(${RTL433_CORE_SRC} ${RTL433_DEVICES_SRC} PROPERTIES
        COMPILE_FLAGS "-w -std=gnu11"
    )
endif()

# Predator's decoder_ingest event types live in core/src/predator/.
target_include_directories(rtl433_decoder PRIVATE
    "${CMAKE_SOURCE_DIR}/core/src"
)
