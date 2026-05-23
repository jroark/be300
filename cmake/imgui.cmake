# Build Dear ImGui (via cimgui's C bindings) into a static library
# `imgui_be300`. The library carries:
#   - imgui.cpp + supporting TUs (core widgets/tables/draw)
#   - cimgui.cpp (C bindings)
#   - cimgui_impl.cpp (C wrappers for ImGui_ImplSDL2_* with CIMGUI_USE_SDL2)
#   - imgui_impl_sdl2.cpp + imgui_impl_sdlrenderer2.cpp (backends)
#
# Consumers (the launcher) link `imgui_be300` and include `cimgui.h` from
# third_party/cimgui plus `launcher/ui_imgui_backend.h` for the SDLRenderer2
# wrappers that cimgui itself does not emit.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/cimgui/imgui/imgui.cpp")
    message(FATAL_ERROR
        "third_party/cimgui/imgui/ is empty.\n"
        "Run: git submodule update --init --recursive")
endif()

enable_language(CXX)
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(_cimgui_root  "${CMAKE_SOURCE_DIR}/third_party/cimgui")
set(_imgui_root   "${_cimgui_root}/imgui")
set(_imgui_bk     "${_imgui_root}/backends")

add_library(imgui_be300 STATIC
    ${_cimgui_root}/cimgui.cpp
    ${_cimgui_root}/cimgui_impl.cpp
    ${_imgui_root}/imgui.cpp
    ${_imgui_root}/imgui_draw.cpp
    ${_imgui_root}/imgui_widgets.cpp
    ${_imgui_root}/imgui_tables.cpp
    ${_imgui_root}/imgui_demo.cpp
    ${_imgui_bk}/imgui_impl_sdl2.cpp
    ${_imgui_bk}/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(imgui_be300 PUBLIC
    ${_cimgui_root}
    ${_imgui_root}
    ${_imgui_bk}
)

# IMGUI_IMPL_API=extern "C" so cimgui_impl.cpp's wrappers link with C names.
# CIMGUI_USE_SDL2 turns on the SDL2 platform backend wrappers in cimgui_impl.
target_compile_definitions(imgui_be300 PUBLIC
    CIMGUI_USE_SDL2
    "IMGUI_IMPL_API=extern \"C\""
)

# imgui's SDL backend needs SDL2 headers visible during its own compilation.
if(SDL2_INCLUDE_DIRS)
    target_include_directories(imgui_be300 PRIVATE ${SDL2_INCLUDE_DIRS})
endif()
# Link SDL2 against imgui_be300 so the launcher's static archive carries
# the transitive dependency in the right link order. Handle both the
# imported-target form (find_package on Linux/macOS) and the raw path form
# (Windows MinGW cross-build with BE300_SDL2_MINGW_ROOT).
if(TARGET SDL2::SDL2)
    target_link_libraries(imgui_be300 PUBLIC SDL2::SDL2)
elseif(SDL2_LIBRARIES)
    target_link_libraries(imgui_be300 PUBLIC ${SDL2_LIBRARIES})
endif()

# Don't warn on imgui's own code style; we don't own it.
target_compile_options(imgui_be300 PRIVATE
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-function
)
