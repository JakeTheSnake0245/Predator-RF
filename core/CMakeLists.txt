cmake_minimum_required(VERSION 3.13)
project(sdrpp_core)

if (USE_INTERNAL_LIBCORRECT)
    add_subdirectory("libcorrect/")
endif (USE_INTERNAL_LIBCORRECT)

if (USE_BUNDLE_DEFAULTS)
add_definitions(-DIS_MACOS_BUNDLE)
endif (USE_BUNDLE_DEFAULTS)

# Main code
file(GLOB_RECURSE SRC "src/*.cpp" "src/*.c")

add_definitions(-DSDRPP_IS_CORE)
add_definitions(-DFLOG_ANDROID_TAG="Predator RF")
if (MSVC)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif ()

# Configure backend sources
if (OPT_BACKEND_GLFW)
    file(GLOB_RECURSE BACKEND_SRC "backends/glfw/*.cpp" "backends/glfw/*.c")
endif (OPT_BACKEND_GLFW)
if (OPT_BACKEND_ANDROID)
    file(GLOB_RECURSE BACKEND_SRC "backends/android/*.cpp" "backends/android/*.c")
    set(BACKEND_SRC ${BACKEND_SRC} ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
endif (OPT_BACKEND_ANDROID)

# Add code to dyn lib
add_library(sdrpp_core SHARED ${SRC} ${BACKEND_SRC})

# Set compiler options
target_compile_options(sdrpp_core PRIVATE ${SDRPP_COMPILER_FLAGS})

# Set the install prefix
target_compile_definitions(sdrpp_core PUBLIC INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}")

# Include core headers
target_include_directories(sdrpp_core PUBLIC "src/")
target_include_directories(sdrpp_core PUBLIC "src/imgui")

# Configure backend includes and libraries
if (OPT_BACKEND_GLFW)
    target_include_directories(sdrpp_core PUBLIC "backends/glfw")
    target_include_directories(sdrpp_core PUBLIC "backends/glfw/imgui")

    if (MSVC)
        # GLFW3
        find_package(glfw3 CONFIG REQUIRED)
        target_link_libraries(sdrpp_core PUBLIC glfw)
    else()
        find_package(PkgConfig)
        pkg_check_modules(GLFW3 REQUIRED glfw3)

        target_include_directories(sdrpp_core PUBLIC ${GLFW3_INCLUDE_DIRS})
        target_link_directories(sdrpp_core PUBLIC ${GLFW3_LIBRARY_DIRS})
        target_link_libraries(sdrpp_core PUBLIC ${GLFW3_LIBRARIES})
    endif()
endif (OPT_BACKEND_GLFW)
if (OPT_BACKEND_ANDROID)
    target_include_directories(sdrpp_core PUBLIC "backends/android")
    target_include_directories(sdrpp_core PUBLIC "backends/android/imgui")
endif (OPT_BACKEND_ANDROID)

# Link to libcorrect
if (USE_INTERNAL_LIBCORRECT)
    target_include_directories(sdrpp_core PUBLIC "libcorrect/include")
    target_link_libraries(sdrpp_core PUBLIC correct_static)
endif (USE_INTERNAL_LIBCORRECT)

if (OPT_OVERRIDE_STD_FILESYSTEM)
    target_include_directories(sdrpp_core PUBLIC "std_replacement")
endif (OPT_OVERRIDE_STD_FILESYSTEM)

if (MSVC)
    # Lib path
    target_link_directories(sdrpp_core PUBLIC "C:/Program Files/PothosSDR/lib/")

    # Misc headers
    target_include_directories(sdrpp_core PUBLIC "C:/Program Files/PothosSDR/include/")

    # Volk
    target_link_libraries(sdrpp_core PUBLIC volk)

    # OpenGL
    find_package(OpenGL REQUIRED)
    target_link_libraries(sdrpp_core PUBLIC OpenGL::GL)

    # GLFW3
    find_package(glfw3 CONFIG REQUIRED)
    target_link_libraries(sdrpp_core PUBLIC glfw)

    # FFTW3
    find_package(FFTW3f CONFIG REQUIRED)
    target_link_libraries(sdrpp_core PUBLIC FFTW3::fftw3f)

    # WinSock2
    target_link_libraries(sdrpp_core PUBLIC wsock32 ws2_32 iphlpapi)

    # ZSTD
    find_package(zstd CONFIG REQUIRED)
    target_link_libraries(sdrpp_core PUBLIC zstd::libzstd_shared)
elseif (ANDROID)
    target_include_directories(sdrpp_core PUBLIC
        ${SDR_KIT_ABI_ROOT}/include
        ${ANDROID_NDK}/sources/android/native_app_glue
    )

    target_link_libraries(sdrpp_core PUBLIC
        ${SDR_KIT_ABI_ROOT}/lib/libvolk.so
        ${SDR_KIT_ABI_ROOT}/lib/libfftw3f.so
        ${SDR_KIT_ABI_ROOT}/lib/libzstd.so
        android
        EGL
        GLESv3
        log
    )
else()
    find_package(PkgConfig)
    find_package(OpenGL REQUIRED)

    pkg_check_modules(FFTW3 REQUIRED fftw3f)
    pkg_check_modules(VOLK REQUIRED volk)
    pkg_check_modules(GLFW3 REQUIRED glfw3)
    pkg_check_modules(LIBZSTD REQUIRED libzstd)

    target_include_directories(sdrpp_core PUBLIC
        ${OPENGL_INCLUDE_DIRS}
        ${FFTW3_INCLUDE_DIRS}
        ${GLFW3_INCLUDE_DIRS}
        ${VOLK_INCLUDE_DIRS}
        ${LIBZSTD_INCLUDE_DIRS}
    )
    
    target_link_directories(sdrpp_core PUBLIC
        ${OPENGL_LIBRARY_DIRS}
        ${FFTW3_LIBRARY_DIRS}
        ${GLFW3_LIBRARY_DIRS}
        ${VOLK_LIBRARY_DIRS}
        ${LIBZSTD_LIBRARY_DIRS}
    )

    target_link_libraries(sdrpp_core PUBLIC
        ${OPENGL_LIBRARIES}
        ${FFTW3_LIBRARIES}
        ${GLFW3_LIBRARIES}
        ${VOLK_LIBRARIES}
        ${LIBZSTD_LIBRARIES}
    )

    if (NOT USE_INTERNAL_LIBCORRECT)
        pkg_check_modules(CORRECT REQUIRED libcorrect)
        target_include_directories(sdrpp_core PUBLIC ${CORRECT_INCLUDE_DIRS})
        target_link_directories(sdrpp_core PUBLIC ${CORRECT_LIBRARY_DIRS})
        target_link_libraries(sdrpp_core PUBLIC ${CORRECT_LIBRARIES})
    endif (NOT USE_INTERNAL_LIBCORRECT)

    if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
        target_link_libraries(sdrpp_core PUBLIC stdc++fs)
    endif ()

endif ()

# OpenSSL — optional. When present, the Kujhad fleet console exposes a
# TLS toggle (with self-signed cert + per-peer pinned fingerprint
# trust). The plain-HTTP path is *always* loopback-only at runtime —
# whether the build has OpenSSL or not, and whether the TLS toggle is
# on or off. The OpenSSL build flag only controls whether HTTPS is
# available; it does not relax the loopback policy on the plain-HTTP
# path. When OpenSSL is missing the TLS UI degrades to a "not built
# with TLS" message and the operator's only option is the plain-HTTP-
# loopback fallback (suitable for local dev or running behind an
# overlay like Tailscale/ZeroTier on the same host). Android skips
# this: the in-repo SDR kit doesn't ship OpenSSL and we don't want to
# enlarge the APK just for the fleet console.
option(OPT_KUJHAD_TLS "Build Kujhad fleet console with TLS (requires OpenSSL)" ON)
if (OPT_KUJHAD_TLS AND NOT ANDROID)
    find_package(OpenSSL)
    if (OpenSSL_FOUND)
        target_compile_definitions(sdrpp_core PUBLIC KUJHAD_HAVE_OPENSSL)
        target_link_libraries(sdrpp_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        message(STATUS "Predator: Kujhad TLS enabled (OpenSSL ${OPENSSL_VERSION})")
    else()
        message(STATUS "Predator: Kujhad TLS disabled (OpenSSL not found)")
    endif()
elseif (ANDROID)
    message(STATUS "Predator: Kujhad TLS disabled on Android (no OpenSSL in SDR kit)")
endif()

set(CORE_FILES ${RUNTIME_OUTPUT_DIRECTORY} PARENT_SCOPE)

# cmake .. "-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake"

# Install directives
install(TARGETS sdrpp_core DESTINATION lib)
