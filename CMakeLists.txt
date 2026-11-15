cmake_minimum_required(VERSION 3.24)
project(Fledge VERSION 27.1.3 LANGUAGES C CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FLEDGE_BUILD_EDITOR "Build the Fledge editor (Dear ImGui)" ON)
# This public source distribution does not include the internal test suite.
set(FLEDGE_BUILD_TESTS OFF)
option(FLEDGE_DXR "Compile the DXR ray-tracing shaders at build time (fetches a pinned DXC release; Windows)" ON)
# This public distribution contains Fledge Standard only.
set(FLEDGE_EDITION "Standard" CACHE STRING "Edition to build: Standard")
set_property(CACHE FLEDGE_EDITION PROPERTY STRINGS Standard)
if(NOT FLEDGE_EDITION STREQUAL "Standard" OR FLEDGE_ADVANCED)
  message(FATAL_ERROR "This source distribution supports only Fledge Standard")
endif()
set(FLEDGE_ADVANCED OFF)
set(FLEDGE_EDITION_ID standard)
set(FLEDGE_EDITION_NAME "Fledge")
set(FLEDGE_PACKAGE_NAME "Fledge")
message(STATUS "Fledge: building the ${FLEDGE_EDITION_NAME} edition (FLEDGE_EDITION=${FLEDGE_EDITION})")

# All executables and DLLs land in one directory so tools can find each other
# (fledgepack locates fledge_runtime.exe next to itself).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(cmake/FledgeOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/Resources.cmake)

add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/render)
add_subdirectory(engine/scene)
add_subdirectory(engine/net)
add_subdirectory(engine/world)
add_subdirectory(engine/anim)
add_subdirectory(engine/physics)
add_subdirectory(engine/audio)
add_subdirectory(engine/gameplay)
add_subdirectory(engine/assets)
add_subdirectory(engine/script)
add_subdirectory(engine/scripting)
add_subdirectory(engine/framework)
add_subdirectory(engine/mcp)
add_subdirectory(engine/ai)
add_subdirectory(engine/blueprint)
add_subdirectory(engine/edition)
set(FLEDGE_EDITION_LIB fledge_edition_standard)

add_subdirectory(runtime)
add_subdirectory(tools/fledgepack)
add_subdirectory(editor/core)
add_subdirectory(tools/mcp_server)
if(FLEDGE_BUILD_EDITOR)
  add_subdirectory(editor/app)
endif()


include(cmake/Install.cmake)

# Enforce: the shipping runtime must never link editor / AI / MCP code.
include(cmake/CheckRuntimeDeps.cmake)
fledge_check_forbidden_deps(fledge_runtime fledge_ai fledge_mcp fledge_editor_core fledge_packager fledge_importers fledge_blueprint cgltf ufbx imgui)
