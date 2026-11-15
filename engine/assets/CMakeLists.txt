# Runtime-safe asset code: registry, paks, cooked formats, materials, PNG writing.
fledge_add_library(fledge_assets)
target_link_libraries(fledge_assets PUBLIC fledge_core PRIVATE stb meshoptimizer)

# Source importers (OBJ, PNG/JPG/TGA/BMP via stb_image, glTF 2.0 via cgltf, FBX via ufbx) and the asset store
# import layer (.unitypackage, Fab .zip, archives, links; fledge/importers/store_import.h).
# Editor/tools only - the shipping runtime must not link this (cooked data only).
file(GLOB_RECURSE _imp_src CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/importers/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/importers/*.h)
add_library(fledge_importers STATIC ${_imp_src})
target_include_directories(fledge_importers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/importers/include)
target_link_libraries(fledge_importers PUBLIC fledge_assets fledge_anim PRIVATE stb cgltf ufbx fledge_audio)
fledge_configure_target(fledge_importers)
