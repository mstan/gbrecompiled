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
    src/debug_server.c
    # Default game hooks — one strong symbol per TU so a partial extras.c
    # overrides cleanly (no multiple-definition, no weak-symbol reliance).
    src/game_defaults/game_default_on_init.c
    src/game_defaults/game_default_on_frame.c
    src/game_defaults/game_default_post_frame.c
    src/game_defaults/game_default_draw_overlay.c
    src/game_defaults/game_default_fill_frame_record.c
    src/game_defaults/game_default_handle_debug_cmd.c
    src/game_defaults/game_default_dispatch_override.c
    src/game_defaults/game_default_get_name.c
    src/game_defaults/game_default_handle_arg.c
    src/game_defaults/game_default_get_expected_crc32.c
    src/game_defaults/game_default_get_valid_crcs.c
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

# Dear ImGui sources (if using SDL2)
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_sources(gbrt PRIVATE
        vendor/imgui/imgui.cpp
        vendor/imgui/imgui_draw.cpp
        vendor/imgui/imgui_tables.cpp
        vendor/imgui/imgui_widgets.cpp
        vendor/imgui/backends/imgui_impl_sdl2.cpp
        vendor/imgui/backends/imgui_impl_opengl3.cpp
    )
    # imgui_impl_opengl3 is dialect-selected at compile time. Force the
    # GLES 2.0 path so one binary covers desktop Mesa, Mali, Adreno,
    # everything we care about.
    set_source_files_properties(
        vendor/imgui/backends/imgui_impl_opengl3.cpp
        PROPERTIES COMPILE_DEFINITIONS "IMGUI_IMPL_OPENGL_ES2"
    )
    target_include_directories(gbrt PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
    )
endif()

target_include_directories(gbrt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

target_compile_features(gbrt PUBLIC c_std_11 cxx_std_17)

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
