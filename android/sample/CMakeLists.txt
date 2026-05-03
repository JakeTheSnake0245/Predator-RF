# ============================================================================
# REFERENCE-ONLY sample CMakeLists.txt
#
# This file is NOT built by the Python backend repo. It documents the
# expected CMake config for the native (C++) decoder modules in the
# Predator-RF Android client at github.com/JakeTheSnake0245/Predator-RF.
#
# Drop the relevant pieces into your real src/main/cpp/CMakeLists.txt;
# the actual repo's decoder_modules/ subtree provides the source files.
# ============================================================================
cmake_minimum_required(VERSION 3.22.1)
project(predator_rf_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Decoder feature flags ────────────────────────────────────────────────────
# Mirror the gradle buildConfigField toggles so a single -D from the gradle
# build flips the right code paths in C++ as well.
option(PREDATOR_ENABLE_DSDFME "Build DSD-FME-derived voice decoder"  ON)
option(PREDATOR_ENABLE_M17    "Build M17 decoder"                    ON)
option(PREDATOR_ENABLE_TETRA  "Build experimental TETRA decoder"     OFF)

# ── Sources (paths assume the JakeTheSnake0245/Predator-RF layout) ──────────
file(GLOB CORE_SRC CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/core/*.cpp
    ${CMAKE_SOURCE_DIR}/core/*.c
)

set(DECODER_SRC "")
if (PREDATOR_ENABLE_DSDFME)
    file(GLOB DSDFME_SRC CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/../../../decoder_modules/dsdfme/*.cpp)
    list(APPEND DECODER_SRC ${DSDFME_SRC})
    add_compile_definitions(PREDATOR_HAS_DSDFME=1)
endif()
if (PREDATOR_ENABLE_M17)
    file(GLOB M17_SRC CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/../../../decoder_modules/m17/*.cpp)
    list(APPEND DECODER_SRC ${M17_SRC})
    add_compile_definitions(PREDATOR_HAS_M17=1)
endif()
if (PREDATOR_ENABLE_TETRA)
    file(GLOB TETRA_SRC CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/../../../decoder_modules/tetra/*.cpp)
    list(APPEND DECODER_SRC ${TETRA_SRC})
    add_compile_definitions(PREDATOR_HAS_TETRA=1)
endif()

add_library(predator_rf SHARED
    ${CORE_SRC}
    ${DECODER_SRC}
)

target_include_directories(predator_rf PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/../../../decoder_modules
)

# Android libraries we always need
find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(predator_rf
    ${log-lib}
    ${android-lib}
)

# ── Build hardening ─────────────────────────────────────────────────────────
target_compile_options(predator_rf PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fstack-protector-strong
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
)
target_link_options(predator_rf PRIVATE
    -Wl,-z,relro -Wl,-z,now -Wl,--gc-sections
)
