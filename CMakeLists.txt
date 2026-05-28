cmake_minimum_required(VERSION 3.20)

project(obs-airplay-source VERSION 0.1.0 LANGUAGES C CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil
  libswscale
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(OBS_APP_PATH "/Applications/OBS.app" CACHE PATH "Path to OBS.app")
set(OBS_SOURCE_DIR "" CACHE PATH "Path to matching obs-studio source checkout for headers")

if(NOT OBS_SOURCE_DIR)
  message(FATAL_ERROR
    "OBS_SOURCE_DIR is required because OBS.app does not ship development headers. "
    "Run scripts/fetch-obs-headers.sh, then configure with "
    "-DOBS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/vendor/obs-studio")
endif()

set(LIBOBS_FRAMEWORK "${OBS_APP_PATH}/Contents/Frameworks/libobs.framework")
if(NOT EXISTS "${LIBOBS_FRAMEWORK}/libobs")
  message(FATAL_ERROR "Could not find libobs.framework at ${LIBOBS_FRAMEWORK}")
endif()

set(OBS_INSTALL_PREFIX "${OBS_APP_PATH}/Contents")
set(OBS_DATA_PATH "Resources/data/obs-studio")
set(OBS_PLUGIN_PATH "PlugIns")
set(OBS_PLUGIN_DESTINATION "PlugIns")
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/obsconfig.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
  @ONLY
)

add_library(obs-airplay-source MODULE
  src/airplay-source.cpp
)

target_include_directories(obs-airplay-source PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_include_directories(obs-airplay-source SYSTEM PRIVATE
  "${OBS_SOURCE_DIR}/libobs"
  "${OBS_SOURCE_DIR}/libobs/util"
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/simde"
)

target_compile_definitions(obs-airplay-source PRIVATE
  OBS_PLUGIN_NAME="obs-airplay-source"
)

target_compile_options(obs-airplay-source PRIVATE
  -Wall
  -Wextra
)

target_link_libraries(obs-airplay-source PRIVATE
  "${LIBOBS_FRAMEWORK}/libobs"
  PkgConfig::FFMPEG
)

set_target_properties(obs-airplay-source PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-airplay-source"
  BUNDLE TRUE
  BUNDLE_EXTENSION "plugin"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
  INSTALL_RPATH "@loader_path/../Frameworks;${OBS_APP_PATH}/Contents/Frameworks"
  BUILD_WITH_INSTALL_RPATH TRUE
)

install(TARGETS obs-airplay-source
  BUNDLE DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins"
  LIBRARY DESTINATION "$ENV{HOME}/Library/Application Support/obs-studio/plugins/obs-airplay-source.plugin/Contents/MacOS"
)
