# Peanut-GB Debug Emulator — Ground truth emulator with TCP debug server
#
# This builds a standalone GB emulator using Peanut-GB (single-header, MIT)
# instrumented with the same TCP debug server protocol as the recompiled runtime.
# Used as ground truth for comparing against the native recompiled build.
#
# Usage:
#   cmake -G Ninja -S tools/peanut-gb-debug -B tools/peanut-gb-debug/build
#   ninja -C tools/peanut-gb-debug/build
#   ./tools/peanut-gb-debug/build/gb-debug-emu roms/tetris.gb

cmake_minimum_required(VERSION 3.20)
project(gb-debug-emu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Find SDL2
find_package(SDL2 REQUIRED)

add_executable(gb-debug-emu
    main.c
)

target_include_directories(gb-debug-emu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/include
)

target_link_libraries(gb-debug-emu PRIVATE SDL2::SDL2)

# Platform-specific
if(WIN32)
    target_link_libraries(gb-debug-emu PRIVATE ws2_32)
    target_compile_definitions(gb-debug-emu PRIVATE _WIN32)
else()
    target_link_libraries(gb-debug-emu PRIVATE m)
endif()

target_compile_definitions(gb-debug-emu PRIVATE
    ENABLE_SOUND=0
    PEANUT_GB_IS_LITTLE_ENDIAN=1
)
