fledge_add_library(fledge_render)
target_link_libraries(fledge_render PUBLIC fledge_core)
# OpenGL functions are loaded at runtime through the window's proc loader;
# opengl32 itself is linked by GLFW.
target_link_libraries(fledge_render PRIVATE fledge_vulkan_headers)
if(WIN32)
  target_link_libraries(fledge_render PRIVATE fledge_directx_headers)
endif()

# DXR shader library -> embedded DXIL (see cmake/Dependencies.cmake "DirectX Shader Compiler")
if(WIN32 AND FLEDGE_DXR AND EXISTS "${FLEDGE_DXC_EXE}")
  set(_gen ${CMAKE_BINARY_DIR}/generated/fledge_render)
  set(_rt ${CMAKE_CURRENT_SOURCE_DIR}/shaders/raytracing.hlsl)
  add_custom_command(OUTPUT ${_gen}/rt_dxil.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen}
    COMMAND ${FLEDGE_DXC_EXE} -T lib_6_3 -O3 -Fo ${_gen}/raytracing.dxil ${_rt}
    COMMAND ${CMAKE_COMMAND} -DIN=${_gen}/raytracing.dxil -DOUT=${_gen}/rt_dxil.h -DNAME=kRaytracingDxil -P ${CMAKE_SOURCE_DIR}/cmake/BinToHeader.cmake
    DEPENDS ${_rt} ${CMAKE_SOURCE_DIR}/cmake/BinToHeader.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "DXC: compiling raytracing.hlsl (lib_6_3)"
    VERBATIM)
  target_sources(fledge_render PRIVATE ${_gen}/rt_dxil.h)
  target_include_directories(fledge_render PRIVATE ${_gen})
  target_compile_definitions(fledge_render PRIVATE FLEDGE_HAVE_DXR_SHADERS=1)
endif()

# AMD FidelityFX FSR 1 shader headers -> embedded text (HLSL, Direct3D 11/12)
if(EXISTS "${FLEDGE_FSR1_DIR}/ffx_fsr1.h")
  set(_gen ${CMAKE_BINARY_DIR}/generated/fledge_render)
  add_custom_command(OUTPUT ${_gen}/ffx_fsr1_text.h ${_gen}/ffx_a_text.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen}
    COMMAND ${CMAKE_COMMAND} -DIN=${FLEDGE_FSR1_DIR}/ffx_a.h -DOUT=${_gen}/ffx_a_text.h -DNAME=kFfxAText -P ${CMAKE_SOURCE_DIR}/cmake/BinToHeader.cmake
    COMMAND ${CMAKE_COMMAND} -DIN=${FLEDGE_FSR1_DIR}/ffx_fsr1.h -DOUT=${_gen}/ffx_fsr1_text.h -DNAME=kFfxFsr1Text -P ${CMAKE_SOURCE_DIR}/cmake/BinToHeader.cmake
    DEPENDS ${FLEDGE_FSR1_DIR}/ffx_a.h ${FLEDGE_FSR1_DIR}/ffx_fsr1.h ${CMAKE_SOURCE_DIR}/cmake/BinToHeader.cmake
    COMMENT "Embedding AMD FidelityFX FSR 1 shader headers"
    VERBATIM)
  target_sources(fledge_render PRIVATE ${_gen}/ffx_fsr1_text.h)
  target_include_directories(fledge_render PRIVATE ${_gen})
  target_compile_definitions(fledge_render PRIVATE FLEDGE_HAVE_FSR1=1)
endif()
if(TARGET fledge_xess_headers)
  target_link_libraries(fledge_render PRIVATE fledge_xess_headers)
endif()
