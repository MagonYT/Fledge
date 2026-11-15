# Unreal Blueprint -> Fledge Script converter (docs/BLUEPRINT_CONVERTER.md). Editor / tools only: the shipping runtime must
# not link it (cmake/CheckRuntimeDeps.cmake). The node mapping table data/node_mappings.json is embedded at configure time.
fledge_add_library(fledge_blueprint)
target_link_libraries(fledge_blueprint PUBLIC fledge_core)

set(_bp_map_h ${CMAKE_BINARY_DIR}/generated/fledge/blueprint/node_mappings_json.h)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/data/node_mappings.json _bp_map)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/node_mappings.json)
file(WRITE ${_bp_map_h}.tmp "#pragma once\n// Generated from engine/blueprint/data/node_mappings.json at configure time - do not edit.\nnamespace fledge::blueprint::data {\ninline constexpr const char* kNodeMappings = R\"BPMAP(${_bp_map})BPMAP\";\n}\n")
configure_file(${_bp_map_h}.tmp ${_bp_map_h} COPYONLY)
target_include_directories(fledge_blueprint PRIVATE ${CMAKE_BINARY_DIR}/generated)
