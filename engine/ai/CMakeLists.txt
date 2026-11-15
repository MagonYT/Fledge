# Editor/tooling-only AI layer. Must never be linked into fledge_runtime.
fledge_add_library(fledge_ai)
target_link_libraries(fledge_ai PUBLIC fledge_core fledge_mcp)
if(WIN32)
  target_link_libraries(fledge_ai PRIVATE winhttp advapi32)
endif()
