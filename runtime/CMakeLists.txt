# The shipping runtime. NEVER add fledge_ai / fledge_mcp / editor libraries here:
# the configure step fails if you do (see cmake/CheckRuntimeDeps.cmake).
set(_runtime_src main.cpp settings_menu.cpp settings_menu.h net_session.cpp net_session.h profile_report.cpp profile_report.h fhtml_overlay.cpp)
# The shipping player uses the Standard edition feature registry.
add_executable(fledge_runtime ${_runtime_src})
target_link_libraries(fledge_runtime PRIVATE fledge_framework fledge_net stb fledge_edition_standard)
fledge_configure_executable(fledge_runtime)
# Console subsystem for the tool copy; fledgepack patches packaged games to GUI (project "subsystem").
fledge_add_version_resource(fledge_runtime "Fledge Runtime (game player)")

set(FLEDGE_CEF_SDK "" CACHE PATH "Optional Windows CEF 152 SDK for the FHTML helper")
if(WIN32 AND FLEDGE_CEF_SDK)
  if(NOT EXISTS "${FLEDGE_CEF_SDK}/Release/bootstrapc.exe" OR
     NOT EXISTS "${FLEDGE_CEF_SDK}/include/capi/cef_app_capi.h")
    message(FATAL_ERROR "FLEDGE_CEF_SDK must point to an extracted CEF Windows SDK with the sandbox bootstrap")
  endif()
  set(_fhtml_dir "${CMAKE_BINARY_DIR}/bin/Fhtml")
  add_library(fledge_fhtml SHARED "${CMAKE_SOURCE_DIR}/tools/fhtml_probe/native.cpp")
  target_include_directories(fledge_fhtml PRIVATE "${FLEDGE_CEF_SDK}")
  target_compile_definitions(fledge_fhtml PRIVATE CEF_API_VERSION=15200)
  target_link_libraries(fledge_fhtml PRIVATE "${FLEDGE_CEF_SDK}/Release/libcef.lib")
  if(MINGW)
    target_link_options(fledge_fhtml PRIVATE -static)
  endif()
  set_target_properties(fledge_fhtml PROPERTIES PREFIX "" OUTPUT_NAME FledgeFhtml RUNTIME_OUTPUT_DIRECTORY "${_fhtml_dir}")
  set(_cef_files chrome_elf.dll d3dcompiler_47.dll dxcompiler.dll dxil.dll libcef.dll
                 v8_context_snapshot.bin vk_swiftshader_icd.json vk_swiftshader.dll vulkan-1.dll)
  list(TRANSFORM _cef_files PREPEND "${FLEDGE_CEF_SDK}/Release/")
  add_custom_command(TARGET fledge_fhtml POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy ${_cef_files} "${_fhtml_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${FLEDGE_CEF_SDK}/Resources" "${_fhtml_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${FLEDGE_CEF_SDK}/Release/bootstrapc.exe" "${_fhtml_dir}/FledgeFhtml.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy "${FLEDGE_CEF_SDK}/LICENSE.txt" "${FLEDGE_CEF_SDK}/CREDITS.html" "${_fhtml_dir}")
  add_dependencies(fledge_runtime fledge_fhtml)
  install(FILES ${_cef_files} "${_fhtml_dir}/FledgeFhtml.exe" "${_fhtml_dir}/FledgeFhtml.dll"
                "${FLEDGE_CEF_SDK}/LICENSE.txt" "${FLEDGE_CEF_SDK}/CREDITS.html" DESTINATION bin/Fhtml COMPONENT Runtime)
  install(DIRECTORY "${FLEDGE_CEF_SDK}/Resources/" DESTINATION bin/Fhtml COMPONENT Runtime)
endif()
