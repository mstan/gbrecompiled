add_executable(gb_snapshot_test snapshot_test.c)
target_link_libraries(gb_snapshot_test PRIVATE gbrt)
add_test(NAME gb_snapshot COMMAND gb_snapshot_test)
set_tests_properties(gb_snapshot PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy;GBRECOMP_STRICT_STATIC=0")

add_executable(gb_debug_json_test debug_json_test.c ../runtime/src/gb_debug_json.c)
target_include_directories(gb_debug_json_test PRIVATE ../runtime/include)
add_test(NAME gb_debug_json COMMAND gb_debug_json_test)

add_executable(gb_launcher_rom_test launcher_rom_test.c)
target_link_libraries(gb_launcher_rom_test PRIVATE gbrt)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/launcher_fixture")
add_test(NAME gb_launcher_rom COMMAND gb_launcher_rom_test "${CMAKE_CURRENT_BINARY_DIR}/launcher_fixture")
set_tests_properties(gb_launcher_rom PROPERTIES TIMEOUT 10)

add_executable(gb_ram_overlay_test ram_overlay_test.cpp
    ../recompiler/src/ram_overlay.cpp ../recompiler/src/rom.cpp)
target_include_directories(gb_ram_overlay_test PRIVATE ../recompiler/include)
add_test(NAME gb_ram_overlay COMMAND gb_ram_overlay_test)

add_executable(gb_jump_table_test jump_table_test.cpp)
target_link_libraries(gb_jump_table_test PRIVATE gbrecomp_core)
add_test(NAME gb_jump_table COMMAND gb_jump_table_test)

add_executable(gb_decoder_memory_test decoder_memory_test.cpp)
target_link_libraries(gb_decoder_memory_test PRIVATE gbrecomp_core)
add_test(NAME gb_decoder_memory COMMAND gb_decoder_memory_test)

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(gb_emit_ram_fixture emit_ram_fixture.cpp)
    target_link_libraries(gb_emit_ram_fixture PRIVATE gbrecomp_core)
    set(_ram_fixture "${CMAKE_CURRENT_BINARY_DIR}/native_ram")
    add_custom_command(OUTPUT "${_ram_fixture}/ram_fixture_native.c"
        COMMAND gb_emit_ram_fixture "${_ram_fixture}"
        DEPENDS gb_emit_ram_fixture VERBATIM)
    add_executable(gb_ram_native_test ram_native_test.c "${_ram_fixture}/ram_fixture_native.c")
    target_include_directories(gb_ram_native_test PRIVATE "${_ram_fixture}")
    target_link_libraries(gb_ram_native_test PRIVATE gbrt)
    add_test(NAME gb_ram_native COMMAND gb_ram_native_test)
    set_tests_properties(gb_ram_native PROPERTIES ENVIRONMENT "GBRECOMP_STRICT_STATIC=0")
endif()

if(GBRT_BOOT_IMPLEMENTATION STREQUAL "LLE")
    add_executable(gb_boot_test boot_test.c)
    target_link_libraries(gb_boot_test PRIVATE gbrt)
    set(GBRECOMP_BOOT_TEST_ROM "" CACHE FILEPATH "Private valid DMG cart for real BIOS handoff tests")
    if(EXISTS "${GBRECOMP_BOOT_TEST_ROM}")
        foreach(model IN ITEMS dmg cgb)
            add_test(NAME gb_boot_${model} COMMAND gb_boot_test ${model}
                "${GBRECOMP_BIOS_DIR}/${model}_boot.bin" "${GBRECOMP_BOOT_TEST_ROM}")
            set_tests_properties(gb_boot_${model} PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_AUDIODRIVER=dummy")
        endforeach()
    endif()
endif()
