cmake_minimum_required(VERSION 3.20)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build universal macOS binaries" FORCE)
endif()

project(reconnect_on_kick VERSION 1.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(TS3_PLUGIN_API_VERSION "26" CACHE STRING "TeamSpeak 3 client plugin API version")
set(DEFAULT_TS3_PLUGIN_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ts3client-pluginsdk")
set(TS3_PLUGIN_SDK_DIR "${DEFAULT_TS3_PLUGIN_SDK_DIR}" CACHE PATH "Path to the TeamSpeak 3 Client Plugin SDK root")

if(NOT EXISTS "${TS3_PLUGIN_SDK_DIR}/include/ts3_functions.h")
    message(FATAL_ERROR "TeamSpeak 3 Client Plugin SDK headers not found. Run scripts/fetch-sdk.sh or set -DTS3_PLUGIN_SDK_DIR=/path/to/ts3client-pluginsdk")
endif()

add_library(reconnect_on_kick SHARED
    src/reconnect_on_kick.c
)

target_include_directories(reconnect_on_kick PRIVATE
    "${TS3_PLUGIN_SDK_DIR}/include"
)

target_compile_definitions(reconnect_on_kick PRIVATE
    TS3_PLUGIN_API_VERSION=${TS3_PLUGIN_API_VERSION}
    TS3_PLUGIN_VERSION="${PROJECT_VERSION}"
)

target_compile_options(reconnect_on_kick PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set_target_properties(reconnect_on_kick PROPERTIES
    PREFIX ""
    OUTPUT_NAME "reconnect_on_kick"
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(APPLE)
    set_target_properties(reconnect_on_kick PROPERTIES SUFFIX ".dylib")
endif()

set(TS3_MACOS_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/TeamSpeak 3/plugins" CACHE PATH "macOS TeamSpeak 3 user plugin directory")

install(TARGETS reconnect_on_kick
    LIBRARY DESTINATION "${TS3_MACOS_PLUGIN_DIR}"
)

set(TS3_PACKAGE_DIR "${CMAKE_BINARY_DIR}/ts3_package")
file(MAKE_DIRECTORY "${TS3_PACKAGE_DIR}")
configure_file(package.ini.in "${TS3_PACKAGE_DIR}/package.ini" @ONLY)

add_custom_target(package-ts3-plugin
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${TS3_PACKAGE_DIR}/plugins"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TS3_PACKAGE_DIR}/plugins"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:reconnect_on_kick>" "${TS3_PACKAGE_DIR}/plugins/$<TARGET_FILE_NAME:reconnect_on_kick>"
    COMMAND "${CMAKE_COMMAND}" -E tar cfv "${CMAKE_BINARY_DIR}/reconnect_on_kick.ts3_plugin" --format=zip package.ini plugins
    WORKING_DIRECTORY "${TS3_PACKAGE_DIR}"
    DEPENDS reconnect_on_kick
    COMMENT "Creating reconnect_on_kick.ts3_plugin"
)
