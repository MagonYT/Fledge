add_library(fledge_packager STATIC src/packager.cpp src/cooker.cpp src/script_build.cpp)
target_include_directories(fledge_packager PUBLIC include)
target_link_libraries(fledge_packager PUBLIC fledge_framework fledge_importers)
target_include_directories(fledge_packager PRIVATE ${CMAKE_SOURCE_DIR}/engine/scripting/sdk)
fledge_configure_target(fledge_packager)

add_executable(fledgepack src/main.cpp)
target_link_libraries(fledgepack PRIVATE fledge_packager ${FLEDGE_EDITION_LIB})
fledge_configure_executable(fledgepack)
fledge_add_version_resource(fledgepack "Fledge Packager (fledgepack)")
# fledgepack needs the runtime + sample module next to it when invoked from bin/.
add_dependencies(fledgepack fledge_runtime)