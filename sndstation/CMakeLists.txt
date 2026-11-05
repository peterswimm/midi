# Gritty Grids for sndstation.
#
# sndstation is built from source alongside the app, so point SNDSTATION_DIR
# at your checkout of https://github.com/sndstation/sndstation:
#
#   cmake -S . -B build -DSNDSTATION_DIR=../../sndstation
#   cmake --build build -j4
#   ./build/gritty-grids
cmake_minimum_required(VERSION 3.16)
project(gritty-grids LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SNDSTATION_DIR "" CACHE PATH "the sndstation source tree")
if(NOT TARGET sndstation::sndstation)
    if(NOT SNDSTATION_DIR)
        message(FATAL_ERROR
            "SNDSTATION_DIR is not set. Clone https://github.com/sndstation/sndstation "
            "and configure with -DSNDSTATION_DIR=/path/to/sndstation")
    endif()
    if(NOT EXISTS "${SNDSTATION_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "SNDSTATION_DIR='${SNDSTATION_DIR}' is not a sndstation tree")
    endif()
    add_subdirectory(${SNDSTATION_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sndstation)
endif()

add_executable(gritty-grids main.cpp)
target_link_libraries(gritty-grids PRIVATE sndstation::sndstation)
target_compile_options(gritty-grids PRIVATE -Wall -Wextra)

# The bytebeat nodes, checked against the web demo's own numbers.
add_executable(test_bytebeat test_bytebeat.cpp)
target_compile_options(test_bytebeat PRIVATE -Wall -Wextra)
# The free lane must reach the speaker unprocessed.
add_executable(test_free test_free.cpp)
target_compile_options(test_free PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME bytebeat COMMAND test_bytebeat)
add_test(NAME free COMMAND test_free)
