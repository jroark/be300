cmake_minimum_required(VERSION 3.10)
project(be300 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
endif()
find_package(SDL2 QUIET)
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

# Generate embedded boot ROM header from binary
set(BOOT_ROM_BIN "${CMAKE_SOURCE_DIR}/docs/hardware/be300_boot_rom.bin")
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
    src/host_io.c
    src/pcconnect.c
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
target_compile_definitions(be300 PRIVATE BE300_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(be300 m ${SDL2_LIBRARIES} ${PNG_LIBRARIES})
if(APPLE)
    target_link_libraries(be300 "-framework Cocoa" "-framework CoreGraphics" "-framework QuartzCore" objc)
endif()

# Tests
enable_testing()
