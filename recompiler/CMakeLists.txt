# Recompiler tool (gbrecomp)

add_library(gbrecomp_core STATIC
    src/config.cpp
    src/rom.cpp
    src/ram_overlay.cpp
    src/decoder.cpp
    src/analyzer.cpp
    src/bank_tracker.cpp
    src/symbol_table.cpp
    src/generator.cpp
    src/ir/ir_builder.cpp
    src/ir/ir_optimizer.cpp
    src/codegen/c_emitter.cpp
    # Reuse the runtime's SHA-1 + SHA-256 so c_emitter can hash the
    # ROM at generation time and bake the digests into the generated
    # code (asset-loader integration uses SHA-1; the launcher-side
    # ROM verification uses SHA-256).
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/gb_sha1.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/gb_sha256.c
    # ...and the runtime's BPS applier, so [rom] patch_file derives a romhack
    # body's image with the exact same code that derives it from the user's ROM
    # at boot (one implementation, no drift).
    ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/src/bps_patch.c
)

target_include_directories(gbrecomp_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus
)

# Enable position-independent code for shared library support
set_target_properties(gbrecomp_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Platform-specific settings
if(APPLE)
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(gbrecomp_core PRIVATE GB_PLATFORM_LINUX)
endif()

find_package(Threads REQUIRED)
target_link_libraries(gbrecomp_core PUBLIC Threads::Threads)
add_executable(gbrecomp src/main.cpp)
target_link_libraries(gbrecomp PRIVATE gbrecomp_core)
target_include_directories(gbrecomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tomlplusplus)
