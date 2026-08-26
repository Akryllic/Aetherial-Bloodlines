if(NOT DEFINED ENV{COMMONLIB_SSE_FOLDER})
  message(FATAL_ERROR "Missing COMMONLIB_SSE_FOLDER environment variable")
endif()

include(cmake/lib/copyOutputs.cmake)
include(cmake/lib/automaticGameFolderOutput.cmake)
include(cmake/lib/autoCollectSources.cmake)

add_src_folder(src)
add_include_folder(include)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

cmake_minimum_required(VERSION 3.21)

set(AUTHOR_NAME "Ivy | Apodiectic")
set(PRODUCT_NAME "AetherialBloodlines")
project(${PRODUCT_NAME} VERSION 1.1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
        @ONLY)

set(RESOURCE_FILE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
set(RESOURCE_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/version.res)

add_custom_command(
    OUTPUT ${RESOURCE_OUTPUT}
    COMMAND rc /fo ${RESOURCE_OUTPUT} ${RESOURCE_FILE}
    DEPENDS ${RESOURCE_FILE}
)

add_custom_target(Resource ALL DEPENDS ${RESOURCE_OUTPUT})

set(ENABLE_SKYRIM_SE OFF CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL " " FORCE)
set(BUILD_TESTS OFF CACHE BOOL " " FORCE)

add_subdirectory("$ENV{COMMONLIB_SSE_FOLDER}" ${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build)
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)

file(
  GLOB_RECURSE
  sources
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc
  ${sources}
)

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${headers} ${sources}
)

include(${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(
  ${PROJECT_NAME}
  AUTHOR ${AUTHOR_NAME}
  SOURCES ${headers} ${sources}
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)
target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE IS_HOST_PLUGIN)

set(wildlander_output false)
set(steam_owrt_output false)
set(steam_mods_output true)
set(lorerim_mods_output false)
set(lorerim3_mods_output true)
automaticGameFolderOutput(wildlander_output, steam_owrt_output, steam_mods_output lorerim_mods_output lorerim3_mods_output)

# Lumina modlist output. automaticGameFolderOutput expects an env var pointing at a *mods root* and
# appends /${PRODUCT_NAME}, which would land in "mods/AetherialBloodlines". This is the mod folder
# itself, so call copyOutputs directly. Override with -DLUMINA_MOD_FOLDER=... if the path moves.
if(NOT DEFINED LUMINA_MOD_FOLDER)
  set(LUMINA_MOD_FOLDER "C:/Lumina/mods/Aetherial Bloodlines - A Race Overhaul")
endif()
if(IS_DIRECTORY "${LUMINA_MOD_FOLDER}")
  copyOutputs("${LUMINA_MOD_FOLDER}")
else()
  message(WARNING "LUMINA_MOD_FOLDER not found, skipping Lumina output: ${LUMINA_MOD_FOLDER}")
endif()
