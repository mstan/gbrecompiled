# runtime/CMakeLists.txt
# GameBoy Runtime Library

add_library(gbrt STATIC
    src/gbrt.c
    src/differential.c
    src/cosim_state.c
    src/interpreter.c
    src/ppu.c
    src/audio.c
    src/audio_stats.c
    src/audio_shadow.c
    src/psg_shadow.c
    src/color_lut.c
    src/debug_server.c
    # Default game hooks — one strong symbol per TU so a partial extras.c
    # overrides cleanly (no multiple-definition, no weak-symbol reliance).
    src/game_defaults/game_default_on_init.c
    src/game_defaults/game_default_on_frame.c
    src/game_defaults/game_default_post_frame.c
    src/game_defaults/game_default_draw_overlay.c
    src/game_defaults/game_default_get_mods.c
    src/game_defaults/game_default_fill_frame_record.c
    src/game_defaults/game_default_handle_debug_cmd.c
    src/game_defaults/game_default_dispatch_override.c
    src/game_defaults/game_default_get_name.c
    src/game_defaults/game_default_get_platform.c
    src/game_defaults/game_default_handle_arg.c
    src/game_defaults/game_default_get_expected_crc32.c
    src/game_defaults/game_default_get_valid_crcs.c
    src/game_defaults/game_default_extended_view.c
    src/gb_widescreen.c
    src/gb_custom_view.c
    src/keybinds.c
    src/launcher.c
    src/platform_sdl.cpp
    src/serial_link.c
    src/network_discovery.c
    src/sgb.c
    src/ir.c
    src/cheats.c
    src/relay_client.c
    src/shader_pipeline.cpp
    src/gb_printer.c
    src/gb_asset_loader.c
    src/gb_sha1.c
    src/gb_sha256.c
    src/bps_patch.c
)

# pthread for the BGB link recv thread
find_package(Threads REQUIRED)
target_link_libraries(gbrt PUBLIC Threads::Threads)

# Online netplay relay client (rendezvous-server HTTP, via libcurl). OFF by
# default: linking libcurl drags in its whole TLS/HTTP2/HTTP3/SSH/brotli DLL
# tree (~16 DLLs). With this off, relay_client.c compiles to small stubs (see
# its #if GBRT_NETPLAY guard) and no curl is linked. Turn on with
# -DGBRT_NETPLAY=ON when netplay is wired up.
option(GBRT_NETPLAY "Build the online netplay relay client (links libcurl)" OFF)
if(GBRT_NETPLAY)
    find_package(CURL REQUIRED)
    target_link_libraries(gbrt PUBLIC CURL::libcurl)
    target_compile_definitions(gbrt PUBLIC GBRT_NETPLAY=1)
endif()

# Game Boy Camera (MBC 0xFC) — webcam capture. Off by default because the
# only cart that needs it is GB-Recomp/gbcamera. Each consumer that needs
# it does `set(GBRT_ENABLE_GBCAM ON CACHE BOOL "" FORCE)` before adding
# gbrt. When off, gbrt.c's MBC 0xFC handler falls through to the built-in
# gradient test pattern.
#
# Per-platform native backends (zero runtime deps — each API ships with
# the OS):
#   - Linux:   v4l2          (src/gbcam.c)
#   - Windows: Media Found.  (src/gbcam.c)
#   - macOS:   AVFoundation  (src/gbcam_macos.m)
#   - Android: TODO — NDK Camera2 API (<camera/NdkCameraDevice.h>, API 24+)
#   - iOS:     TODO — AVFoundation (src/gbcam_macos.m is mostly portable,
#                                   the AVFoundation API is identical)
#
# OpenCV is an alternative single-codepath backend (src/gbcam.cpp). Not
# wired here because it adds a ~30 MB runtime dependency for end users
# and gave us garbled frames on the v4l2 path. Enable manually if needed.
option(GBRT_ENABLE_GBCAM "Build Game Boy Camera webcam capture (native backends)" OFF)
if(GBRT_ENABLE_GBCAM)
    target_sources(gbrt PRIVATE src/gbcam.c)
    target_compile_definitions(gbrt PUBLIC GBRT_HAVE_GBCAM=1)
    if(APPLE)
        target_sources(gbrt PRIVATE src/gbcam_macos.m)
        target_link_libraries(gbrt PRIVATE "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
    endif()
endif()

# A launcher-enabled build shares the pinned UI's ImGui with the runtime.
# This keeps one core/backend copy and avoids drifting header/API versions.
option(GBRECOMP_RECOMP_UI "Build the recomp-ui pre-boot launcher" ON)
option(RECOMP_UI_ENABLE_MODS "Enable game-owned Mods in recomp-ui" OFF)
set(RECOMP_UI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../recomp-ui" CACHE PATH
    "Root of the recomp-ui launcher checkout")
set(GBRECOMP_LAUNCHER_CONSOLE "gbc" CACHE STRING "Launcher asset family: gb or gbc")
set(GBRT_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui")
if(GBRECOMP_RECOMP_UI AND EXISTS "${RECOMP_UI_ROOT}/src/third_party/imgui/imgui.h")
    set(GBRT_IMGUI_DIR "${RECOMP_UI_ROOT}/src/third_party/imgui")
endif()
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_sources(gbrt PRIVATE
        ${GBRT_IMGUI_DIR}/imgui.cpp
        ${GBRT_IMGUI_DIR}/imgui_draw.cpp
        ${GBRT_IMGUI_DIR}/imgui_tables.cpp
        ${GBRT_IMGUI_DIR}/imgui_widgets.cpp
        ${GBRT_IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
        ${GBRT_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    set_source_files_properties(${GBRT_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        PROPERTIES COMPILE_DEFINITIONS "IMGUI_IMPL_OPENGL_ES2")
    target_include_directories(gbrt PUBLIC ${GBRT_IMGUI_DIR})
    target_include_directories(gbrt PRIVATE ${GBRT_IMGUI_DIR}/backends)
endif()
target_include_directories(gbrt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
target_compile_features(gbrt PUBLIC c_std_11 cxx_std_17)

if(GBRECOMP_RECOMP_UI AND SDL2_FOUND AND EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
    if(WIN32)
        # Resolve shared GL 1.1 names from ANGLE before the helper's desktop
        # opengl32 fallback. Mixing those providers gives a black launcher.
        target_link_libraries(gbrt PUBLIC GLESv2 EGL)
    endif()
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    recomp_target_launcher_ui(gbrt CONSOLE ${GBRECOMP_LAUNCHER_CONSOLE}
        HOST_IMGUI IMGUI_DIR ${GBRT_IMGUI_DIR})
    target_sources(gbrt PRIVATE src/launcher_ui_seam.c)
    # gbrt owns the stb_image_write implementation and the GLES2 context.
    target_compile_definitions(gbrt PRIVATE RECOMP_UI_HOST_STB_WRITE LNG_GLES2)
    # Keep the known MinGW COMDAT/section-GC workaround on launcher TUs.
    if(NOT MSVC)
        get_target_property(_gb_sources gbrt SOURCES)
        foreach(_gb_source IN LISTS _gb_sources)
            string(FIND "${_gb_source}" "${RECOMP_UI_ROOT}/src/" _gb_is_ui)
            if(_gb_is_ui EQUAL 0 OR _gb_source STREQUAL "src/launcher_ui_seam.c")
                set_source_files_properties(${_gb_source} PROPERTIES
                    COMPILE_OPTIONS "-fno-data-sections;-fno-function-sections")
            endif()
        endforeach()
    endif()
    message(STATUS "recomp-ui launcher: ON (${RECOMP_UI_ROOT}), Mods=${RECOMP_UI_ENABLE_MODS}")
elseif(GBRECOMP_RECOMP_UI)
    message(STATUS "recomp-ui launcher: OFF (not found at ${RECOMP_UI_ROOT} or no SDL2)")
endif()

# Debug mode option
option(GB_DEBUG "Enable debug logging" OFF)
option(GB_DEBUG_VRAM "Enable VRAM debug logging" OFF)
option(GB_DEBUG_FRAME "Enable frame debug logging" OFF)
option(GB_DEBUG_REGS "Enable register debug logging" OFF)
option(GB_DEBUG_PPU "Enable PPU debug logging" OFF)

if(GB_DEBUG)
    target_compile_definitions(gbrt PUBLIC GB_DEBUG)
    message(STATUS "GameBoy debug logging enabled")
endif()

if(GB_DEBUG_VRAM)
    target_compile_definitions(gbrt PUBLIC GB_DEBUG_VRAM)
endif()
if(GB_DEBUG_FRAME)
    target_compile_definitions(gbrt PUBLIC GB_DEBUG_FRAME)
endif()
if(GB_DEBUG_REGS)
    target_compile_definitions(gbrt PUBLIC GB_DEBUG_REGS)
endif()
if(GB_DEBUG_PPU)
    target_compile_definitions(gbrt PUBLIC GB_DEBUG_PPU)
endif()

# Platform-specific settings
if(APPLE)
    target_compile_definitions(gbrt PRIVATE GB_PLATFORM_MACOS)
elseif(WIN32)
    target_compile_definitions(gbrt PRIVATE GB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(gbrt PRIVATE GB_PLATFORM_LINUX)
endif()

# SDL2 for platform layer (optional)
if(SDL2_FOUND)
    target_link_libraries(gbrt PUBLIC SDL2::SDL2)
    target_compile_definitions(gbrt PUBLIC GB_HAS_SDL2)

    # GL stack for the post-process shader pipeline + ImGui's opengl3
    # backend. We compile against the GLES 2.0 subset so the same code
    # runs on desktop Mesa and embedded GLES (Mali on Orange Pi).
    if(APPLE)
        target_link_libraries(gbrt PUBLIC "-framework OpenGL")
    elseif(WIN32)
        # GLES 2.0 entrypoints (glUseProgram, glBufferData, ...) are provided
        # by ANGLE (libGLESv2/libEGL), not opengl32 which only exposes GL 1.1.
        # The GLES2 headers mark these dllimport, so the ANGLE import libs are
        # required at link time; the matching DLLs ship next to the binary.
        target_link_libraries(gbrt PUBLIC GLESv2 EGL)
    else()
        find_package(OpenGL QUIET)
        if(TARGET OpenGL::GL)
            target_link_libraries(gbrt PUBLIC OpenGL::GL)
        elseif(TARGET OpenGL::OpenGL)
            target_link_libraries(gbrt PUBLIC OpenGL::OpenGL)
        else()
            target_link_libraries(gbrt PUBLIC GL)
        endif()
    endif()
endif()

# Winsock for debug server TCP; comdlg32 for the launcher's file picker (GetOpenFileName)
if(WIN32)
    target_link_libraries(gbrt PUBLIC ws2_32 comdlg32)
endif()

# ---------------------------------------------------------------------------
# Embedded SameBoy oracle (cross-oracle co-simulation; see COSIM_ORACLE.md).
# OFF by default — production/normal builds never link SameBoy. Enable with
#   -DGBC_COSIM_SAMEBOY=ON -DSAMEBOY_DIR=<path to SameBoy source tree>
# Builds SameBoy's Core (all features on — do NOT pass -DGB_DISABLE_*, which
# needs SameBoy's CPPP preprocessor) plus our sameboy_oracle.c driver into a
# static lib, links it into gbrt, and defines GBC_HAVE_SAMEBOY.
# ---------------------------------------------------------------------------
option(GBC_COSIM_SAMEBOY "Build the embedded SameBoy oracle" OFF)
set(SAMEBOY_DIR "F:/Projects/SameBoy" CACHE PATH "Path to the SameBoy source tree (has Core/)")
if(GBC_COSIM_SAMEBOY)
    if(NOT EXISTS "${SAMEBOY_DIR}/Core/gb.h")
        message(FATAL_ERROR "GBC_COSIM_SAMEBOY=ON but ${SAMEBOY_DIR}/Core/gb.h not found; set -DSAMEBOY_DIR=")
    endif()
    file(GLOB SAMEBOY_CORE_SRC "${SAMEBOY_DIR}/Core/*.c")
    add_library(sameboy_core STATIC ${SAMEBOY_CORE_SRC} src/sameboy_oracle.c)
    target_include_directories(sameboy_core PRIVATE
        "${SAMEBOY_DIR}/Core"
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(sameboy_core PRIVATE _GNU_SOURCE GB_INTERNAL GB_VERSION="cosim")
    target_compile_options(sameboy_core PRIVATE -std=gnu11 -w -Wno-implicit-function-declaration)
    target_link_libraries(gbrt PUBLIC sameboy_core)
    target_compile_definitions(gbrt PUBLIC GBC_HAVE_SAMEBOY=1)
    message(STATUS "SameBoy oracle: ON (${SAMEBOY_DIR})")
endif()
