fledge_add_library(fledge_audio)
target_link_libraries(fledge_audio PUBLIC fledge_core PRIVATE miniaudio)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
