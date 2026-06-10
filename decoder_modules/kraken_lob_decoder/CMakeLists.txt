cmake_minimum_required(VERSION 3.13)
project(kraken_lob_decoder)

# -----------------------------------------------------------------------------
# Predator RF: KrakenSDR LOB (Line of Bearing) decoder module
#
#   Bridges to a running krakensdr_doa companion process via WebSocket
#   (ws://host:8082/ws, default).  No vendored third-party code — the
#   WebSocket client is implemented inline in decoder_ingest.h using only
#   POSIX sockets (Linux/Android) or Winsock2 (Windows).
#
#   The module:
#     1. Registers as a SDRPP module (appears in Module Manager).
#     2. Maintains a background WebSocket thread that connects to
#        krakensdr_doa and streams JSON LOB results.
#     3. Converts each result into a DecoderIngestEvent (decoder="KRAKEN_LOB")
#        and enqueues it for the UI thread.
#     4. The predator bridge ingester picks up KRAKEN_LOB events and calls
#        backend.fusion.lob_triangulator via the existing HTTP/IPC path.
#
#   Dependencies: none beyond SDRPP core (no Boost, no external WS lib).
# -----------------------------------------------------------------------------

file(GLOB_RECURSE SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(kraken_lob_decoder PRIVATE
    "src/"
    "${CMAKE_SOURCE_DIR}/core/src"
)
