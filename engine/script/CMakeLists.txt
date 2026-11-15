fledge_add_library(fledge_script)
target_link_libraries(fledge_script PUBLIC fledge_scene)
if(NOT WIN32)
  target_link_libraries(fledge_script PRIVATE ${CMAKE_DL_LIBS})
endif()

# Header-only interface target for gameplay modules: they only see the C ABI.
add_library(fledge_module_sdk INTERFACE)
target_include_directories(fledge_module_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
