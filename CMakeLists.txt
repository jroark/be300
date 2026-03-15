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

# Compiler flags
add_compile_options(-Wall -Wextra -g -O2)

# Find Unicorn
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(UNICORN QUIET unicorn)
    pkg_check_modules(SDL2 QUIET sdl2)
endif()

if(NOT UNICORN_FOUND)
    # Robust fallback for systems where pkg-config is not set up correctly
    # Only searching standard system paths, NOT third_party
    find_path(UNICORN_INCLUDE_DIRS NAMES unicorn/unicorn.h
        HINTS /opt/homebrew/include /usr/local/include /usr/include
    )
    find_library(UNICORN_LIBRARIES NAMES unicorn
        HINTS /opt/homebrew/lib /usr/local/lib /usr/lib
    )
    if(UNICORN_INCLUDE_DIRS AND UNICORN_LIBRARIES)
        set(UNICORN_FOUND TRUE)
        message(STATUS "Found Unicorn via fallback: ${UNICORN_LIBRARIES}")
    else()
        message(FATAL_ERROR "Unicorn not found! Please install it (e.g., 'brew install unicorn' or 'apt install libunicorn-dev').")
    endif()
endif()

# Include directories
find_package(SDL2 QUIET)
include_directories(src ${UNICORN_INCLUDE_DIRS})
if(SDL2_FOUND OR SDL2_LIBRARIES)
    include_directories(${SDL2_INCLUDE_DIRS})
    add_compile_definitions(HAVE_SDL2)
    message(STATUS "SDL2 found — building with display support")
else()
    message(STATUS "SDL2 not found — building headless (no display)")
endif()

# Source files for the core emulator logic (shared between main and tests)
set(BE300_CORE_SOURCES
    src/machine.c
    src/wince_diag.c
    src/wince_init.c
    src/bus.c
    src/loader.c
    src/macc.c
    src/ui.c
    src/hw/bcu.c
    src/hw/cmu.c
    src/hw/pmu.c
    src/hw/icu.c
    src/hw/siu.c
    src/hw/rtc.c
    src/hw/gpio.c
    src/hw/nand.c
)

# Main executable
add_executable(be300 src/main.c ${BE300_CORE_SOURCES})
target_link_libraries(be300 ${UNICORN_LIBRARIES} ${SDL2_LIBRARIES})

# Tests
enable_testing()

add_executable(test_basic tests/test_basic.c ${BE300_CORE_SOURCES})
target_link_libraries(test_basic ${UNICORN_LIBRARIES} ${SDL2_LIBRARIES})

add_test(NAME basic_test COMMAND test_basic)
