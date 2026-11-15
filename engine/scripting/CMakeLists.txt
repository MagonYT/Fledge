# Fledge Script - engine side (runtime-safe: no editor / AI code).
#   sdk/fledge/script/*.h   public SDK headers compiled into script modules (installed to <root>/sdk/include)
#   include/fledge/script/  host API used by the engine (ScriptHost, InputMap)
fledge_add_library(fledge_scripting)
target_include_directories(fledge_scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
target_link_libraries(fledge_scripting PUBLIC fledge_scene)
if(NOT WIN32)
  target_link_libraries(fledge_scripting PRIVATE ${CMAKE_DL_LIBS})
endif()

# Header-only SDK target (tests compile the module runtime directly).
add_library(fledge_script_sdk INTERFACE)
target_include_directories(fledge_script_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
