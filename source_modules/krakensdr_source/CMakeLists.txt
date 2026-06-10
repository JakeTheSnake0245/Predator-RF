cmake_minimum_required(VERSION 3.13)
project(krakensdr_source)

# -----------------------------------------------------------------------------
# Predator RF: KrakenSDR Source stub module
#
#   KrakenSDR does NOT expose raw IQ samples to SDR++ — it processes the
#   5-channel coherent IQ internally and outputs DOA results only via its
#   REST / WebSocket API (krakensdr_doa).  This stub registers in the
#   SDRPP source selector and displays an informational panel explaining
#   the architecture and the required krakensdr_doa companion process.
#
#   To receive LOB bearing output: enable the kraken_lob_decoder module,
#   which connects to krakensdr_doa's WebSocket and ingests DOA results
#   into the Predator RF event pipeline.
# -----------------------------------------------------------------------------

file(GLOB_RECURSE SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(krakensdr_source PRIVATE
    "src/"
    "${CMAKE_SOURCE_DIR}/core/src"
)
