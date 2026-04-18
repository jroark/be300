cmake_minimum_required(VERSION 3.10)
project(be300 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_WEB "Build the WebAssembly frontend bundle" OFF)

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

# SDL2 (optional, for framebuffer display)
if(EMSCRIPTEN)
    message(STATUS "Emscripten build — forcing headless UI path")
else()
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
    endif()
    find_package(SDL2 QUIET)
    if(SDL2_FOUND OR SDL2_LIBRARIES)
        include_directories(${SDL2_INCLUDE_DIRS})
        add_compile_definitions(HAVE_SDL2)
        message(STATUS "SDL2 found — building with display support")
    else()
        message(STATUS "SDL2 not found — building headless (no display)")
    endif()
endif()

# Generate embedded boot ROM header from binary
set(BOOT_ROM_BIN "${CMAKE_SOURCE_DIR}/docs/be300_boot_rom.bin")
set(BOOT_ROM_HDR "${CMAKE_BINARY_DIR}/boot_rom_embedded.h")
add_custom_command(
    OUTPUT "${BOOT_ROM_HDR}"
    COMMAND ${CMAKE_COMMAND} -E env
        sh ${CMAKE_SOURCE_DIR}/tools/gen_rom_header.sh
            ${BOOT_ROM_BIN} ${BOOT_ROM_HDR}
    DEPENDS "${BOOT_ROM_BIN}"
    COMMENT "Generating embedded boot ROM header"
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
    src/be300_devices.c
    src/host_io.c
    src/loader.c
    src/ui.c
    src/hw/bcu.c
    src/hw/cmu.c
    src/hw/pmu.c
    src/hw/icu.c
    src/hw/siu.c
    src/hw/rtc.c
    src/hw/gpio.c
    src/hw/nand.c
    src/hw/cf.c
)

# Main executable
add_executable(be300 src/main.c ${GXEMUL_SOURCES} ${BE300_SOURCES} ${BOOT_ROM_HDR})
target_link_libraries(be300 m ${SDL2_LIBRARIES})

if(BUILD_WEB)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "BUILD_WEB requires an Emscripten toolchain")
    endif()

    set(WEB_OUTPUT_DIR ${CMAKE_BINARY_DIR}/web)
    file(MAKE_DIRECTORY ${WEB_OUTPUT_DIR})
    file(MAKE_DIRECTORY ${WEB_OUTPUT_DIR}/kernels)
    foreach(asset index.html app.js worker.js styles.css be300_frame.png favicon.svg)
        configure_file(${CMAKE_SOURCE_DIR}/web/${asset}
                       ${WEB_OUTPUT_DIR}/${asset}
                       COPYONLY)
    endforeach()
    foreach(kernel_asset vmlinux-pgui-demo vmlinux-mw vmlinux-2.6)
        configure_file(${CMAKE_SOURCE_DIR}/kernels/${kernel_asset}
                       ${WEB_OUTPUT_DIR}/kernels/${kernel_asset}
                       COPYONLY)
    endforeach()

    add_executable(be300_web ${GXEMUL_SOURCES} ${BE300_SOURCES} ${BOOT_ROM_HDR})
    set_target_properties(be300_web PROPERTIES
        OUTPUT_NAME "be300_web"
        RUNTIME_OUTPUT_DIRECTORY ${WEB_OUTPUT_DIR}
    )
    target_link_libraries(be300_web m)
    target_link_options(be300_web PRIVATE
        "--no-entry"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sENVIRONMENT=worker"
        "-sEXPORT_ES6=1"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createBe300Module"
        "-sNO_EXIT_RUNTIME=1"
        "-sFILESYSTEM=0"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_be300_create_web','_be300_boot_linux_from_memory','_be300_step','_be300_copy_frame_rgba8888','_be300_drain_serial','_be300_set_touch','_be300_set_buttons','_be300_stop','_be300_destroy']"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8','HEAPU32']"
    )
endif()

# Tests
enable_testing()
