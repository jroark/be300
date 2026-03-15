cmake_minimum_required(VERSION 3.22.1)
project(be300_android C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(BE300_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(BE300_SRC ${BE300_ROOT}/src)

set(BE300_CORE_SOURCES
    ${BE300_SRC}/android_bridge.c
    ${BE300_SRC}/machine.c
    ${BE300_SRC}/bus.c
    ${BE300_SRC}/loader.c
    ${BE300_SRC}/macc.c
    ${BE300_SRC}/ui.c
    ${BE300_SRC}/hw/bcu.c
    ${BE300_SRC}/hw/cmu.c
    ${BE300_SRC}/hw/pmu.c
    ${BE300_SRC}/hw/icu.c
    ${BE300_SRC}/hw/siu.c
    ${BE300_SRC}/hw/rtc.c
    ${BE300_SRC}/hw/gpio.c
)

add_library(be300_jni SHARED ${BE300_CORE_SOURCES})

target_include_directories(be300_jni PRIVATE ${BE300_SRC})
target_compile_options(be300_jni PRIVATE -Wall -Wextra -O2)

set(UNICORN_INCLUDE_DIR ${BE300_ROOT}/android/unicorn/include)
set(UNICORN_LIB ${BE300_ROOT}/android/unicorn/${ANDROID_ABI}/libunicorn.so)

if(NOT EXISTS ${UNICORN_INCLUDE_DIR}/unicorn/unicorn.h)
    message(FATAL_ERROR "Missing Unicorn headers for Android at ${UNICORN_INCLUDE_DIR}.")
endif()

if(NOT EXISTS ${UNICORN_LIB})
    message(FATAL_ERROR "Missing Unicorn Android library at ${UNICORN_LIB}.")
endif()

add_library(unicorn SHARED IMPORTED)
set_target_properties(unicorn PROPERTIES
    IMPORTED_LOCATION ${UNICORN_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${UNICORN_INCLUDE_DIR}
)

target_include_directories(be300_jni PRIVATE ${UNICORN_INCLUDE_DIR})

find_library(log-lib log)
find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)

target_link_libraries(be300_jni
    unicorn
    ${log-lib}
    ${android-lib}
    ${jnigraphics-lib}
)
