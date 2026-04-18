cmake_minimum_required(VERSION 3.22.1)
project(be300_android C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(BE300_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(BE300_SRC ${BE300_ROOT}/src)
set(GXEMUL_SRC ${BE300_ROOT}/gxemul/src)

# ============================================================
# GXemul MIPS CPU engine sources (mirrors top-level CMakeLists.txt)
# ============================================================
set(GXEMUL_SOURCES
    # CPU core
    ${GXEMUL_SRC}/cpus/cpu.c
    ${GXEMUL_SRC}/cpus/cpu_mips.c
    ${GXEMUL_SRC}/cpus/cpu_mips_coproc.c
    ${GXEMUL_SRC}/cpus/cpu_mips_instr_unaligned.c

    # Core framework
    ${GXEMUL_SRC}/core/memory.c
    ${GXEMUL_SRC}/core/interrupt.c
    ${GXEMUL_SRC}/core/timer.c
    ${GXEMUL_SRC}/core/settings.c
    ${GXEMUL_SRC}/core/misc.c
    ${GXEMUL_SRC}/core/debugmsg.c
    ${GXEMUL_SRC}/core/float_emul.c

    # Console (serial I/O)
    ${GXEMUL_SRC}/console/console.c

    # Devices
    ${GXEMUL_SRC}/devices/device.c
    ${GXEMUL_SRC}/devices/dev_vr41xx.c
    ${GXEMUL_SRC}/devices/dev_ram.c
    ${GXEMUL_SRC}/devices/dev_ns16550.c
    ${GXEMUL_SRC}/devices/dev_fb.c

    # Symbol table
    ${GXEMUL_SRC}/symbol/symbol.c

    # Machine registry and BE-300 machine definition
    ${GXEMUL_SRC}/machines/machine.c
    ${GXEMUL_SRC}/machines/machine_hpcmips.c

    # Stubs for unused subsystems (X11, debugger, diskimage, etc.)
    ${GXEMUL_SRC}/stubs.c
)

# ============================================================
# BE-300 emulator sources
# ============================================================
set(BE300_CORE_SOURCES
    # JNI bridge (lives alongside this CMakeLists.txt)
    ${CMAKE_CURRENT_SOURCE_DIR}/android_bridge.c

    # Machine core
    ${BE300_SRC}/machine_be300.c
    ${BE300_SRC}/be300_devices.c
    ${BE300_SRC}/host_io.c
    ${BE300_SRC}/ui.c

    # Hardware peripherals
    ${BE300_SRC}/hw/bcu.c
    ${BE300_SRC}/hw/cmu.c
    ${BE300_SRC}/hw/pmu.c
    ${BE300_SRC}/hw/icu.c
    ${BE300_SRC}/hw/siu.c
    ${BE300_SRC}/hw/rtc.c
    ${BE300_SRC}/hw/gpio.c
    ${BE300_SRC}/hw/nand.c
)

add_library(be300_jni SHARED ${BE300_CORE_SOURCES} ${GXEMUL_SOURCES})

target_include_directories(be300_jni PRIVATE
    ${BE300_SRC}
    ${GXEMUL_SRC}/include
)

target_compile_options(be300_jni PRIVATE
    -Wall -O2
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
)

find_library(log-lib log)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)

target_link_libraries(be300_jni
    m
    ${log-lib}
    ${android-lib}
    ${jnigraphics-lib}
)
