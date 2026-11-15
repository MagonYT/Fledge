fledge_add_library(fledge_core)
target_include_directories(fledge_core PUBLIC ${CMAKE_BINARY_DIR}/generated)  # fledge/core/version.h
target_link_libraries(fledge_core PUBLIC nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(fledge_core PUBLIC Threads::Threads)
