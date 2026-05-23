# macOS .app bundling. Produces ${CMAKE_BINARY_DIR}/BE300.app and copies it
# into dist/BE300.app for downstream packaging (tools/build_macos_installer.sh
# wraps it into a .pkg).
#
# Usage from the top-level CMakeLists:
#     include(cmake/macos_bundle.cmake)
#
# Requires: the `be300` executable target to already exist.
#
# Dylib bundling: tries `dylibbundler` if it's on PATH. If absent, leaves the
# binary referencing whatever it was linked against — fine for local use but
# not for distribution.

if(NOT APPLE)
    return()
endif()

if(NOT BE300_VERSION)
    set(BE300_VERSION "0.1.0")
endif()

set(BE300_APP_NAME    "BE300")
set(BE300_APP_DIR     "${CMAKE_BINARY_DIR}/${BE300_APP_NAME}.app")
set(BE300_APP_MACOS   "${BE300_APP_DIR}/Contents/MacOS")
set(BE300_APP_RES     "${BE300_APP_DIR}/Contents/Resources")
set(BE300_DIST_DIR    "${CMAKE_BINARY_DIR}/../dist")

# Resolve the bundled Info.plist content from the template.
set(BE300_PLIST_IN    "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in")
set(BE300_PLIST_OUT   "${CMAKE_BINARY_DIR}/Info.plist")
configure_file(${BE300_PLIST_IN} ${BE300_PLIST_OUT} @ONLY)

set(BE300_ICNS        "${CMAKE_SOURCE_DIR}/packaging/macos/be300.icns")
set(BE300_ICONSET_DIR "${CMAKE_SOURCE_DIR}/packaging/macos/icon.iconset")

# Best-effort generation of the .icns from icon.iconset if iconutil is
# available and the .icns doesn't already exist. Designers regenerate by
# editing icon.iconset/ and running tools/build_macos_icon.sh.
add_custom_command(
    OUTPUT ${BE300_ICNS}
    COMMAND ${CMAKE_COMMAND} -E echo "Generating ${BE300_ICNS} from icon.iconset/"
    COMMAND iconutil -c icns ${BE300_ICONSET_DIR} -o ${BE300_ICNS} || true
    DEPENDS ${BE300_ICONSET_DIR}
    COMMENT "Generating macOS .icns from icon.iconset/"
    VERBATIM
)

add_custom_target(package_macos
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${BE300_APP_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BE300_APP_MACOS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BE300_APP_RES}
    COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:be300> ${BE300_APP_MACOS}/be300
    COMMAND ${CMAKE_COMMAND} -E copy
            ${BE300_PLIST_OUT} ${BE300_APP_DIR}/Contents/Info.plist
    # Copy an icns if one was committed; ignore failure for first-time builds
    # without one.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${BE300_ICNS} ${BE300_APP_RES}/be300.icns || true
    # dylibbundler is best-effort. Install with `brew install dylibbundler`.
    COMMAND bash -c "if command -v dylibbundler >/dev/null 2>&1; then \
            dylibbundler -of -b -cd \
                -x ${BE300_APP_MACOS}/be300 \
                -d ${BE300_APP_DIR}/Contents/libs \
                -p '@executable_path/../libs/'; \
        else \
            echo '[package_macos] dylibbundler not found; skipping dylib bundling.'; \
            echo '[package_macos] brew install dylibbundler to make the .app portable.'; \
        fi"
    # Drop into dist/
    COMMAND ${CMAKE_COMMAND} -E make_directory ${BE300_DIST_DIR}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${BE300_DIST_DIR}/${BE300_APP_NAME}.app
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${BE300_APP_DIR} ${BE300_DIST_DIR}/${BE300_APP_NAME}.app
    DEPENDS be300
    COMMENT "Building ${BE300_APP_NAME}.app"
    VERBATIM)
