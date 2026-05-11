# runtime/CMakeLists.txt
# GameBoy Runtime Library

add_library(gbrt STATIC
    src/gbrt.c
    src/differential.c
    src/interpreter.c
    src/ppu.c
    src/audio.c
    src/audio_stats.c
    src/platform_sdl.cpp
    src/gbcam.c
    src/serial_link.c
    src/network_discovery.c
    src/sgb.c
    src/ir.c
    src/mock_ir.c
    src/relay_client.c
    src/shader_pipeline.cpp
    src/gb_printer.c
    src/gb_asset_loader.c
    src/gb_sha1.c
)

# pthread for the BGB link recv thread
find_package(Threads REQUIRED)
target_link_libraries(gbrt PUBLIC Threads::Threads)

# libcurl for the rendezvous-server HTTP client
find_package(CURL REQUIRED)
target_link_libraries(gbrt PUBLIC CURL::libcurl)

# macOS: add AVFoundation camera backend
if(APPLE)
    target_sources(gbrt PRIVATE src/gbcam_macos.m)
    target_link_libraries(gbrt PRIVATE "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo")
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
        target_link_libraries(gbrt PUBLIC opengl32)
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
