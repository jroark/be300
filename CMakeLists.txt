cmake_minimum_required(VERSION 3.10)
project(be300 C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Auto-detect Apple Silicon even if running in Rosetta
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    execute_process(COMMAND sysctl -n hw.optional.arm64 OUTPUT_VARIABLE IS_ARM64_HW OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(IS_ARM64_HW STREQUAL "1")
        message(STATUS "Detected Apple Silicon hardware. Defaulting CMAKE_OSX_ARCHITECTURES to arm64.")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build for arm64" FORCE)
    endif()
endif()

# Compiler flags — relax warnings for GXemul legacy code
add_compile_options(-Wall -g -O2
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
)

if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
        SDL_MAIN_HANDLED
    )
endif()

# SDL2 (optional, for framebuffer display)
set(BE300_SDL2_MINGW_ROOT "" CACHE PATH
    "SDL2 x86_64-w64-mingw32 prefix for Windows cross-builds")
if(NOT ANDROID)
    if(WIN32 AND BE300_SDL2_MINGW_ROOT)
        set(SDL2_FOUND TRUE)
        set(SDL2_INCLUDE_DIRS "${BE300_SDL2_MINGW_ROOT}/include/SDL2")
        set(SDL2_LIBRARIES "${BE300_SDL2_MINGW_ROOT}/lib/libSDL2.dll.a")
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(SDL2 QUIET sdl2)
        endif()
        find_package(SDL2 QUIET)
    endif()
    if(TARGET SDL2::SDL2 AND NOT WIN32)
        set(SDL2_LIBRARIES SDL2::SDL2)
    endif()
    if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES "arm64"
            AND EXISTS "/opt/homebrew/include/png.h"
            AND EXISTS "/opt/homebrew/lib/libpng.dylib")
        set(PNG_PNG_INCLUDE_DIR "/opt/homebrew/include" CACHE PATH
            "Path to arm64 libpng headers" FORCE)
        set(PNG_LIBRARY_RELEASE "/opt/homebrew/lib/libpng.dylib" CACHE FILEPATH
            "Path to arm64 libpng library" FORCE)
    endif()
    find_package(PNG QUIET)
    if(SDL2_FOUND OR SDL2_LIBRARIES)
        include_directories(${SDL2_INCLUDE_DIRS})
        add_compile_definitions(HAVE_SDL2)
        message(STATUS "SDL2 found — building with display support")
        if(PNG_FOUND)
            include_directories(${PNG_INCLUDE_DIRS})
            add_compile_definitions(HAVE_PNG)
            message(STATUS "PNG found — building with SDL frame support")
        else()
            message(STATUS "PNG not found — SDL frame support disabled")
        endif()
    else()
        message(STATUS "SDL2 not found — building headless (no display)")
    endif()
else()
    message(STATUS "Android build — SDL2 host display disabled")
endif()

# Generate embedded headers from binary assets via xxd -i.
set(EMBED_GEN "${CMAKE_SOURCE_DIR}/tools/gen_embedded_header.sh")

set(BOOT_ROM_BIN "${CMAKE_SOURCE_DIR}/docs/hardware/be300_boot_rom.bin")
set(BOOT_ROM_HDR "${CMAKE_BINARY_DIR}/boot_rom_embedded.h")
add_custom_command(
    OUTPUT "${BOOT_ROM_HDR}"
    COMMAND sh ${EMBED_GEN} ${BOOT_ROM_BIN} ${BOOT_ROM_HDR} be300_boot_rom
    DEPENDS "${BOOT_ROM_BIN}" "${EMBED_GEN}"
    COMMENT "Generating embedded boot ROM header"
)

set(FRAME_PNG_SRC "${CMAKE_SOURCE_DIR}/src/ui/assets/be300_frame.png")
set(FRAME_PNG_HDR "${CMAKE_BINARY_DIR}/frame_png_embedded.h")
add_custom_command(
    OUTPUT "${FRAME_PNG_HDR}"
    COMMAND sh ${EMBED_GEN} ${FRAME_PNG_SRC} ${FRAME_PNG_HDR} be300_frame_png
    DEPENDS "${FRAME_PNG_SRC}" "${EMBED_GEN}"
    COMMENT "Generating embedded frame PNG header"
)

set(BUTTONS_MASK_PNG_SRC "${CMAKE_SOURCE_DIR}/src/ui/assets/buttons_dpad_bw_mask.png")
set(BUTTONS_MASK_PNG_HDR "${CMAKE_BINARY_DIR}/buttons_mask_png_embedded.h")
add_custom_command(
    OUTPUT "${BUTTONS_MASK_PNG_HDR}"
    COMMAND sh ${EMBED_GEN} ${BUTTONS_MASK_PNG_SRC} ${BUTTONS_MASK_PNG_HDR} be300_buttons_mask_png
    DEPENDS "${BUTTONS_MASK_PNG_SRC}" "${EMBED_GEN}"
    COMMENT "Generating embedded button mask PNG header"
)

set(EMBEDDED_ASSET_HEADERS
    ${BOOT_ROM_HDR}
    ${FRAME_PNG_HDR}
    ${BUTTONS_MASK_PNG_HDR}
)

# Include directories
include_directories(
    src
    gxemul/src/include
    ${CMAKE_BINARY_DIR}
)

# ============================================================
# GXemul MIPS CPU engine sources
# ============================================================
set(GXEMUL_SOURCES
    # CPU core — only these 4 files are compiled standalone.
    # cpu_mips.c #includes tmp_mips_head.c, tmp_mips_tail.c, cpu_dyntrans.c,
    # memory_mips.c, memory_rw.c, cpu_mips_instr.c, tmp_mips_loadstore.c,
    # tmp_mips_loadstore_multi.c, and memory_mips_v2p.c as templates.
    gxemul/src/cpus/cpu.c
    gxemul/src/cpus/cpu_mips.c
    gxemul/src/cpus/cpu_mips16.c
    gxemul/src/cpus/cpu_mips_coproc.c
    gxemul/src/cpus/cpu_mips_instr_unaligned.c

    # Core framework
    gxemul/src/core/memory.c
    gxemul/src/core/interrupt.c
    gxemul/src/core/timer.c
    gxemul/src/core/settings.c
    gxemul/src/core/misc.c
    gxemul/src/core/debugmsg.c
    gxemul/src/core/float_emul.c

    # Networking backend used by the BE-300 PCMCIA NE2000 card.
    gxemul/src/net/net.c
    gxemul/src/net/net_ether.c
    gxemul/src/net/net_ip.c
    gxemul/src/net/net_misc.c
    gxemul/src/net/net_tap.c

    # Console (serial I/O via stdin/stdout)
    gxemul/src/console/console.c

    # Devices
    gxemul/src/devices/device.c
    gxemul/src/devices/dev_vr41xx.c
    gxemul/src/devices/dev_ram.c
    gxemul/src/devices/dev_ns16550.c
    gxemul/src/devices/dev_fb.c

    # Symbol table (for debug output)
    gxemul/src/symbol/symbol.c

    # Machine registry and machine definitions
    gxemul/src/machines/machine.c
    gxemul/src/machines/machine_hpcmips.c

    # Stubs for unused subsystems
    gxemul/src/stubs.c
)

# ============================================================
# BE-300 emulator sources
# ============================================================
set(BE300_SOURCES
    src/machine_be300.c
    src/be300_probe.c
    src/be300_devices.c
    src/win32_compat.c
    src/devices/input.c
    src/devices/nand_window.c
    src/devices/pcmcia_attr.c
    src/devices/siu_extra.c
    src/devices/wince_aux.c
    src/host_io.c
    src/pcconnect.c
    src/pcconnect_bridge.c
    src/stowaway.c
    src/ui.c
    src/ui_stowaway.c
    src/hw/bcu.c
    src/hw/cmu.c
    src/hw/pmu.c
    src/hw/icu.c
    src/hw/siu.c
    src/hw/rtc.c
    src/hw/gpio.c
    src/hw/nand.c
    src/hw/nand_restore.c
    src/hw/cf.c
    src/hw/cf_ne2000.c
    src/hw/aiu.c
)

# ============================================================
# Launcher UI (VMware/UTM-style VM manager). Default ON for the host CLI,
# forced OFF for web/Android builds (those targets reuse the emulator core
# but have their own frontends and don't need an embedded launcher).
# Phase 0: thin stub that prints a banner. Later phases add Dear ImGui
# (via cimgui), a VM-bundle store under ~/Library/Application Support/BE300/VMs/,
# and per-platform .app/.pkg/.exe/AppImage packaging.
# ============================================================
option(BUILD_LAUNCHER "Build the in-process VM launcher UI" ON)
if(BUILD_ANDROID OR BUILD_WEB)
    set(BUILD_LAUNCHER OFF CACHE BOOL "" FORCE)
endif()

set(BE300_LAUNCHER_SOURCES)
if(BUILD_LAUNCHER)
    list(APPEND BE300_LAUNCHER_SOURCES
        src/launcher/launcher_main.c
        src/launcher/ui_manager.c
        src/launcher/ui_wizard.c
        src/launcher/ui_settings.c
        src/launcher/ui_filepick.c
        src/launcher/ui_imgui_backend.cpp
        src/launcher/vm_bundle.c
        src/launcher/vm_config_json.c
        third_party/cjson/cJSON.c
    )
    include_directories(third_party/cjson)
    include(${CMAKE_SOURCE_DIR}/cmake/imgui.cmake)

    # nativefiledialog-extended (NFDe) — native file pickers per OS.
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nfde/CMakeLists.txt")
        set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory(third_party/nfde EXCLUDE_FROM_ALL)
        set(BE300_NFD_TARGET nfd)
    else()
        message(WARNING
            "third_party/nfde missing — file picker disabled.\n"
            "Run: git submodule update --init --recursive")
        set(BE300_NFD_TARGET "")
    endif()
endif()

# Native host CLI executable.
option(BUILD_CLI "Build the host command-line emulator" ON)
if(BUILD_CLI)
    add_executable(be300
        src/main.c
        ${GXEMUL_SOURCES}
        ${BE300_SOURCES}
        ${BE300_LAUNCHER_SOURCES}
        ${EMBEDDED_ASSET_HEADERS})
    target_link_libraries(be300 m ${SDL2_LIBRARIES} ${PNG_LIBRARIES})
    if(BUILD_LAUNCHER)
        target_compile_definitions(be300 PRIVATE BE300_HAVE_LAUNCHER=1)
        target_link_libraries(be300 imgui_be300)
        if(BE300_NFD_TARGET)
            target_link_libraries(be300 ${BE300_NFD_TARGET})
            target_include_directories(be300 PRIVATE
                ${CMAKE_SOURCE_DIR}/third_party/nfde/src/include)
        endif()
        target_include_directories(be300 PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/cimgui
            ${CMAKE_SOURCE_DIR}/third_party/cimgui/imgui
            ${CMAKE_SOURCE_DIR}/third_party/cimgui/imgui/backends)
        target_compile_definitions(be300 PRIVATE CIMGUI_USE_SDL2)
    endif()
    if(WIN32)
        target_compile_options(be300 PRIVATE
            "-include" "${CMAKE_SOURCE_DIR}/src/win32_compat.h")
        target_link_libraries(be300 ws2_32)
        # Static MinGW runtime so the .exe runs on a clean Windows host
        # without libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll
        # next to it. The launcher pulls in C++ (cimgui/imgui/NFDe), so
        # libstdc++ is mandatory now; -static also covers winpthread which
        # NFDe and imgui both touch.
        target_link_options(be300 PRIVATE
            -static-libgcc -static-libstdc++ -static)
    endif()
    if(APPLE)
        target_link_libraries(be300 "-framework Cocoa" "-framework CoreGraphics" "-framework QuartzCore" objc)
    endif()
endif()

# Android JNI shared library. The Android app drives the same embeddable core
# API used by the web frontend, with SDL disabled and rendering done in Java.
option(BUILD_ANDROID "Build the Android JNI shared library" OFF)
if(BUILD_ANDROID)
    if(NOT ANDROID)
        message(FATAL_ERROR "BUILD_ANDROID requires the Android NDK toolchain")
    endif()

    add_library(be300_android SHARED
        android/app/src/main/cpp/be300_jni.c
        ${GXEMUL_SOURCES} ${BE300_SOURCES} ${EMBEDDED_ASSET_HEADERS})
    target_compile_definitions(be300_android PRIVATE
        BE300_ANDROID=1)
    target_link_libraries(be300_android m log android jnigraphics)
endif()

# ============================================================
# Optional Emscripten/WebAssembly frontend (additive only).
#
# The browser bundle reuses the emulator core verbatim and adds one
# Emscripten-only translation unit (web/main_web.c) on top of the
# existing GXEMUL_SOURCES + BE300_SOURCES lists. Configuring with
# `cmake -DBUILD_WEB=ON ..` from a non-Emscripten toolchain is a
# user error and produces FATAL_ERROR; the supported entry point is
# `./tools/build_web.sh`, which invokes `emcmake cmake`.
# ============================================================
option(BUILD_WEB "Build the WebAssembly frontend bundle" OFF)

if(BUILD_WEB)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "BUILD_WEB requires an Emscripten toolchain (emcmake cmake ..)")
    endif()

    set(WEB_OUTPUT_DIR ${CMAKE_BINARY_DIR}/web)
    file(MAKE_DIRECTORY ${WEB_OUTPUT_DIR})
    foreach(asset index.html app.js worker.js styles.css favicon.svg)
        configure_file(${CMAKE_SOURCE_DIR}/web/${asset}
                       ${WEB_OUTPUT_DIR}/${asset}
                       COPYONLY)
    endforeach()
    foreach(asset be300_frame.png buttons_dpad_bw_mask.png)
        configure_file(${CMAKE_SOURCE_DIR}/src/ui/assets/${asset}
                       ${WEB_OUTPUT_DIR}/${asset}
                       COPYONLY)
    endforeach()
    file(MAKE_DIRECTORY ${WEB_OUTPUT_DIR}/nand)
    file(GLOB WEB_NAND_ASSETS CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/nand/*.bin)
    foreach(asset ${WEB_NAND_ASSETS})
        get_filename_component(asset_name ${asset} NAME)
        configure_file(${asset}
                       ${WEB_OUTPUT_DIR}/nand/${asset_name}
                       COPYONLY)
    endforeach()

    add_executable(be300_web
        ${CMAKE_SOURCE_DIR}/web/main_web.c
        ${GXEMUL_SOURCES} ${BE300_SOURCES} ${EMBEDDED_ASSET_HEADERS})
    set_target_properties(be300_web PROPERTIES
        OUTPUT_NAME "be300_web"
        RUNTIME_OUTPUT_DIRECTORY ${WEB_OUTPUT_DIR})
    target_compile_options(be300_web PRIVATE "-sUSE_SDL=2")
    target_link_libraries(be300_web m)
    target_link_options(be300_web PRIVATE
        "-sUSE_SDL=2"
        "--no-entry"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sENVIRONMENT=worker"
        "-sEXPORT_ES6=1"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createBe300Module"
        "-sNO_EXIT_RUNTIME=1"
        "-sFORCE_FILESYSTEM=1"
        "-sSTACK_SIZE=2MB"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_be300_web_create','_be300_web_create_ex','_be300_web_create_ex2','_be300_web_load_nand','_be300_web_load_cf','_be300_web_stowaway_key','_be300_web_set_net_bridge_enabled','_be300_web_net_rx','_be300_web_net_tx_pending','_be300_web_net_tx_pop','_be300_step','_be300_copy_frame_rgba8888','_be300_drain_serial','_be300_set_touch','_be300_set_buttons','_be300_stop','_be300_destroy']"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32','FS','cwrap','ccall']")
endif()

if(APPLE AND BUILD_CLI)
    include(${CMAKE_SOURCE_DIR}/cmake/macos_bundle.cmake)
endif()

# Tests
enable_testing()

if(BUILD_LAUNCHER AND BUILD_CLI)
    add_executable(test_vm_config
        tests/test_vm_config.c
        src/launcher/vm_bundle.c
        src/launcher/vm_config_json.c
        third_party/cjson/cJSON.c)
    target_include_directories(test_vm_config PRIVATE src third_party/cjson)
    add_test(NAME vm_config COMMAND test_vm_config)
endif()
