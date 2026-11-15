fledge_add_library(fledge_editor_core)
target_link_libraries(fledge_editor_core PUBLIC fledge_framework fledge_packager fledge_importers fledge_mcp fledge_ai fledge_blueprint fledge_net PRIVATE stb)
if(WIN32)
  # link IPC (Winsock, TCP owner lookup, BCrypt token), protocol handlers (registry), known folders
  target_link_libraries(fledge_editor_core PRIVATE ws2_32 iphlpapi bcrypt advapi32 shell32 ole32)
endif()

# Fledge Script docs embedded for the MCP resources fledge://script/api and fledge://script/examples
# (so they work in any install, not just next to the docs folder).
set(_script_docs_h ${CMAKE_BINARY_DIR}/generated/fledge/editor/script_docs.h)
# Documentation is optional in the public source export.
set(_script_api "Script API documentation is not bundled with this source distribution.")
set(_script_examples "Script examples are not bundled with this source distribution.")
foreach(_doc api examples)
  set(_path ${CMAKE_SOURCE_DIR}/docs/script/${_doc}.md)
  if(EXISTS ${_path})
    file(READ ${_path} _script_${_doc})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_path})
  endif()
endforeach()
file(WRITE ${_script_docs_h}.tmp "#pragma once\n// Generated from docs/script/*.md at configure time - do not edit.\nnamespace fledge::scriptdocs {\ninline constexpr const char* kApi = R\"SCRIPTDOC(${_script_api})SCRIPTDOC\";\ninline constexpr const char* kExamples = R\"SCRIPTDOC(${_script_examples})SCRIPTDOC\";\n}\n")
configure_file(${_script_docs_h}.tmp ${_script_docs_h} COPYONLY)
target_include_directories(fledge_editor_core PUBLIC ${CMAKE_BINARY_DIR}/generated)

# Engine system docs embedded for the MCP resources fledge://docs/<name> (AI tools read them; docs/AI_HANDBOOK.md
# is the entry point once it exists).
set(_engine_docs_h ${CMAKE_BINARY_DIR}/generated/fledge/editor/engine_docs.h)
set(_engine_docs_body "")
set(_engine_doc_names WORLD_STREAMING ANIMATION PHYSICS AUDIO GAMEPLAY FHTML NETWORKING ASSET_STORES AI_HANDBOOK BLUEPRINT_CONVERTER MIGRATING_SETTINGS)
foreach(_doc ${_engine_doc_names})
  set(_path ${CMAKE_SOURCE_DIR}/docs/${_doc}.md)
  if(EXISTS ${_path})
    file(READ ${_path} _text)
    string(TOLOWER ${_doc} _id)
    string(APPEND _engine_docs_body "    {\"${_id}\", R\"ENGDOC(${_text})ENGDOC\"},\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_path})
  endif()
endforeach()
file(WRITE ${_engine_docs_h}.tmp "#pragma once\n// Generated from docs/*.md at configure time - do not edit.\n#include <utility>\n#include <initializer_list>\nnamespace fledge::enginedocs {\ninline const std::initializer_list<std::pair<const char*, const char*>> kDocs = {\n${_engine_docs_body}};\n}\n")
configure_file(${_engine_docs_h}.tmp ${_engine_docs_h} COPYONLY)
