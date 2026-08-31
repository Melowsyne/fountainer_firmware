cmake_minimum_required(VERSION 3.16.0)

# version.txt -> PROJECT_VER -> esp_app_desc_t.version (reported firmware version,
# the value the server compares against for OTA). IMPORTANT: file(READ ...) is only
# evaluated at CMake *configure* time, so a changed version.txt is picked up only when
# CMake reconfigures. CMAKE_CONFIGURE_DEPENDS requests that reconfigure -- but some build
# front-ends (notably PlatformIO) do NOT reliably honour it on an incremental build, so
# the image can ship a version that lags version.txt -> the server keeps offering the
# same OTA forever (infinite loop). The authoritative guard therefore lives in flash.sh,
# which reads the version actually embedded in the built .bin and forces a clean rebuild
# on mismatch. Keep this dependency anyway -- it fixes the case for plain `idf.py`.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/version.txt")
file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

# An empty PROJECT_VER would silently break the OTA version comparison. Fail loudly
# instead of building an image that reports "" as its version.
if(PROJECT_VER STREQUAL "")
    message(FATAL_ERROR "version.txt is missing or empty -- refusing to build with an "
                        "empty PROJECT_VER (would break the OTA version comparison).")
endif()
message(STATUS "Fountain firmware PROJECT_VER = ${PROJECT_VER} (from version.txt)")

# The reusable components (datapoints, clientside_protocol) live under
# src/components/ instead of the default search path <project>/components.
# APPEND (not set!): PlatformIO already supplies the src/ directory as the
# main component via this variable — overwriting it would remove the
# main target ("Couldn't find the main target of the project").
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/src/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(FountainRegulatorESP)
