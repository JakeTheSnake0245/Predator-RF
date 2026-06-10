cmake_minimum_required(VERSION 3.13)
project(predator_rfctl)

add_executable(predator-rfctl "main.cpp")
target_compile_options(predator-rfctl PRIVATE ${SDRPP_COMPILER_FLAGS})
target_include_directories(predator-rfctl PRIVATE "${CMAKE_SOURCE_DIR}/core/src")

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    target_link_libraries(predator-rfctl PRIVATE pthread)
endif ()

install(TARGETS predator-rfctl DESTINATION bin)
