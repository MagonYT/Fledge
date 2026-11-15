# Relocatable install layout + CPack portable ZIP.
#
#   <prefix>/bin/        Fledge (editor), fledge_runtime, fledgepack, fledge_mcp_server
#   <prefix>/templates/  project templates (New Project wizard)
#   <prefix>/licenses/   third-party licenses
#   <prefix>/resources/  fonts + icons
#   <prefix>/sdk/include Fledge Script SDK headers (scripts are compiled against these)
#   <prefix>/tools/browser_extension  "Send to Fledge" extension (chromium/, firefox/), loaded unpacked
#
# Executables are linked statically (MinGW -static), so no compiler runtime
# DLLs are needed.
#
#   cmake --install build/ninja-gcc-release --prefix <dir>     # relocatable install (user data: %APPDATA%\Fledge)
#   cmake --build build/ninja-gcc-release --target package      # dist/Fledge-<ver>-win64.zip (portable)
set(_targets fledge_runtime fledgepack fledge_mcp_server)
if(FLEDGE_BUILD_EDITOR)
  list(APPEND _targets fledge_editor)
endif()
install(TARGETS ${_targets} RUNTIME DESTINATION bin COMPONENT Runtime)
if(FLEDGE_DXC_DLL)  # HLSL -> SPIR-V at runtime (Vulkan backend); copied next to packaged games by fledgepack
  install(FILES ${FLEDGE_DXC_DLL} DESTINATION bin COMPONENT Runtime)
endif()

install(DIRECTORY ${CMAKE_SOURCE_DIR}/templates/ DESTINATION templates COMPONENT Runtime
        PATTERN "Intermediate" EXCLUDE PATTERN "Build" EXCLUDE
        PATTERN "compile_commands.json" EXCLUDE PATTERN ".clangd" EXCLUDE REGEX "Scripts/CMakeLists\\.txt$" EXCLUDE)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/licenses/ DESTINATION licenses COMPONENT Runtime)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/engine/scripting/sdk/ DESTINATION sdk/include COMPONENT Runtime)  # Fledge Script SDK headers
install(DIRECTORY ${FLEDGE_RESOURCES_BUILD_DIR}/ DESTINATION resources COMPONENT Runtime)
# "Send to Fledge" browser extension sources (Chrome / Edge MV3, Firefox), loaded unpacked by the user
install(DIRECTORY ${CMAKE_SOURCE_DIR}/tools/browser_extension/ DESTINATION tools/browser_extension COMPONENT Runtime)
install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md
        DESTINATION . COMPONENT Runtime)

# --- CPack: portable ZIP --------------------------------------------------------------
set(CPACK_PACKAGE_NAME "${FLEDGE_EDITION_NAME}")
set(CPACK_PACKAGE_VENDOR "Fledge Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${FLEDGE_EDITION_NAME} (editor, runtime, packager, MCP server)")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_SYSTEM_NAME "win64")
set(CPACK_PACKAGE_FILE_NAME "${FLEDGE_PACKAGE_NAME}-${PROJECT_VERSION}-win64")
set(CPACK_GENERATOR "ZIP")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_OUTPUT_FILE_PREFIX ${CMAKE_BINARY_DIR}/dist)
# The ZIP is portable: drop portable.txt into the staged tree (not into plain `cmake --install`).
set(FLEDGE_PORTABLE_TXT ${CMAKE_SOURCE_DIR}/cmake/templates/portable.txt)
configure_file(${CMAKE_SOURCE_DIR}/cmake/templates/cpack_portable.cmake.in ${CMAKE_BINARY_DIR}/cpack_portable.cmake @ONLY)
set(CPACK_PRE_BUILD_SCRIPTS ${CMAKE_BINARY_DIR}/cpack_portable.cmake)
include(CPack)

# --- NSIS installer (optional; fetches a pinned, checksum-verified NSIS 3.10 zip into the build dir) ---
#   cmake --build build/ninja-gcc-release --target installer   ->  dist/Fledge-<ver>-setup.exe
add_custom_target(installer
  COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
          -DVERSION=${PROJECT_VERSION} -DCMAKE_COMMAND_PATH=${CMAKE_COMMAND}
          -DEDITION=${FLEDGE_EDITION} -DPACKAGE_NAME=${FLEDGE_PACKAGE_NAME}
          -P ${CMAKE_SOURCE_DIR}/cmake/BuildNsisInstaller.cmake
  USES_TERMINAL
  COMMENT "Building the NSIS installer")
add_dependencies(installer ${_targets})
