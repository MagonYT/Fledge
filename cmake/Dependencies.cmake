# Third-party dependencies, all pinned. Sources are fetched into
# ${CMAKE_BINARY_DIR}/_deps and treated as untrusted data: we only compile
# them, we never run their scripts.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- nlohmann/json (header only) -------------------------------------------
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# --- stb (image load/write), pinned commit ----------------------------------
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG d14d46f6b44456eb88beac58cba833a4135d4c89
  GIT_SHALLOW FALSE)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
  FetchContent_Populate(stb)
endif()
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# --- cgltf (glTF 2.0 loader, MIT, single header) - editor/tools only ------
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
  GIT_TAG v1.15
  GIT_SHALLOW TRUE)
FetchContent_GetProperties(cgltf)
if(NOT cgltf_POPULATED)
  FetchContent_Populate(cgltf)
endif()
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})

# --- ufbx v0.23.1 (MIT or public domain, single file): FBX import - editor/tools only (SHA-256 pinned archive) ---
# Only ufbx.c / ufbx.h are compiled; the archive's tests, data and scripts are never run.
FetchContent_Declare(ufbx
  URL https://github.com/ufbx/ufbx/archive/refs/tags/v0.23.1.tar.gz
  URL_HASH SHA256=21edd1021dfb430e37aa6214c9b3bbaa5634b1859cbbef7231e738af4f19c956
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(ufbx)
if(NOT ufbx_POPULATED)
  FetchContent_Populate(ufbx)
endif()
add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
target_compile_options(ufbx PRIVATE -w)

# --- meshoptimizer 0.22 (MIT): LOD simplification (SHA-256 pinned archive) ---------------
FetchContent_Declare(meshoptimizer
  URL https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.22.tar.gz
  URL_HASH SHA256=e296cf0685b6421f84bd8a44d0a3cca82a219500f11c793dfbb6087ec86bb1a3
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(meshoptimizer)
if(NOT meshoptimizer_POPULATED)
  FetchContent_Populate(meshoptimizer)
endif()
file(GLOB _meshopt_src ${meshoptimizer_SOURCE_DIR}/src/*.cpp)
add_library(meshoptimizer STATIC ${_meshopt_src})
target_include_directories(meshoptimizer SYSTEM PUBLIC ${meshoptimizer_SOURCE_DIR}/src)

# --- Jolt Physics 5.2.0 (MIT): rigid bodies, characters, vehicles, ragdolls (SHA-256 pinned archive) ---------
# Cross-platform determinism on (Time Machine replays), no AVX2/AVX-512/FMA so games run on any x64 CPU with SSE4.2.
foreach(_opt TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER ENABLE_ALL_WARNINGS
        INTERPROCEDURAL_OPTIMIZATION USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD ENABLE_INSTALL
        DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE FLOATING_POINT_EXCEPTIONS_ENABLED GENERATE_DEBUG_SYMBOLS
        USE_STATIC_MSVC_RUNTIME_LIBRARY DOUBLE_PRECISION OVERRIDE_CXX_FLAGS)
  set(${_opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
  URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.2.0.tar.gz
  URL_HASH SHA256=f478afe3050c885e21403748e10ab18e3e8df8b0982c540e75f1e078ef8b2c88
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(JoltPhysics)
if(TARGET Jolt)
  target_compile_options(Jolt PRIVATE -w)  # third-party: keep our build output warning-free
endif()

# --- miniaudio 0.11.21 (Public Domain / MIT-0): audio device output + WAV/MP3/FLAC decoding (SHA-256 pinned) ---
FetchContent_Declare(miniaudio
  URL https://github.com/mackron/miniaudio/archive/refs/tags/0.11.21.tar.gz
  URL_HASH SHA256=6afb5c231613d2fab4f1c668b7243ff9a7d6d78a7f5a2692c133f026fe508506
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
  FetchContent_Populate(miniaudio)
endif()
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})

# --- Recast & Detour 1.6.0 (zlib): navigation meshes + path queries (SHA-256 pinned archive) -------------------
FetchContent_Declare(recastnavigation
  URL https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz
  URL_HASH SHA256=d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(recastnavigation)
if(NOT recastnavigation_POPULATED)
  FetchContent_Populate(recastnavigation)
endif()
file(GLOB _recast_src ${recastnavigation_SOURCE_DIR}/Recast/Source/*.cpp ${recastnavigation_SOURCE_DIR}/Detour/Source/*.cpp)
add_library(recastnav STATIC ${_recast_src})
target_include_directories(recastnav SYSTEM PUBLIC ${recastnavigation_SOURCE_DIR}/Recast/Include ${recastnavigation_SOURCE_DIR}/Detour/Include)
target_compile_options(recastnav PRIVATE -w)

# --- GLFW 3.4 ---------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# --- Dear ImGui (docking branch) -------------------------------------------
if(FLEDGE_BUILD_EDITOR)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
  endif()
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
  target_link_libraries(imgui PUBLIC glfw)
  if(WIN32)
    target_sources(imgui PRIVATE ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
    target_link_libraries(imgui PUBLIC opengl32 d3dcompiler)
  endif()
endif()

# --- doctest ----------------------------------------------------------------
if(FLEDGE_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(doctest)
  if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
  endif()
  add_library(doctest INTERFACE)
  target_include_directories(doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})
endif()

# --- Vulkan-Headers (Apache-2.0 / MIT), headers only ------------------------
# The Vulkan backend loads vulkan-1.dll at runtime (VK_NO_PROTOTYPES); nothing is linked.
FetchContent_Declare(vulkan_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.367.tar.gz
  URL_HASH SHA256=0df7d58b5df617e0a6a17dec521af459206719f9c7cb12d05d09275597c16f70
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(vulkan_headers)
if(NOT vulkan_headers_POPULATED)
  FetchContent_Populate(vulkan_headers)
endif()
add_library(fledge_vulkan_headers INTERFACE)
target_include_directories(fledge_vulkan_headers SYSTEM INTERFACE ${vulkan_headers_SOURCE_DIR}/include)
target_compile_definitions(fledge_vulkan_headers INTERFACE VK_NO_PROTOTYPES)

# --- DirectX-Headers (MIT), headers only ------------------------------------
# Newer d3d12.h than MinGW ships (DXR / ID3D12Device5, enhanced barriers). Only the D3D12 backend
# includes <directx/...>; d3d12.dll is still loaded at runtime (nothing is linked).
if(WIN32)
  FetchContent_Declare(directx_headers
    URL https://github.com/microsoft/DirectX-Headers/archive/refs/tags/v1.620.1.tar.gz
    URL_HASH SHA256=79cd6d5b87d940044b20a3f45e5fbf4a73e4c7eedc1bafabe1b5cabb97fef5bf
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(directx_headers)
  if(NOT directx_headers_POPULATED)
    FetchContent_Populate(directx_headers)
  endif()
  add_library(fledge_directx_headers INTERFACE)
  target_include_directories(fledge_directx_headers SYSTEM INTERFACE ${directx_headers_SOURCE_DIR}/include)
endif()

# --- DirectX Shader Compiler ------------------------------------------------
# Build time: compiles engine/render/shaders/raytracing.hlsl (DXR, lib_6_3) to DXIL, embedded into fledge_render.
# Runtime: dxcompiler.dll (HLSL -> SPIR-V for the Vulkan backend) ships next to the editor, the tools and every
# packaged game. dxil.dll is not needed (SPIR-V is not signed).
if(WIN32)
  FetchContent_Declare(dxc
    URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip
    URL_HASH SHA256=ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(dxc)
  if(NOT dxc_POPULATED)
    FetchContent_Populate(dxc)
  endif()
  set(FLEDGE_DXC_EXE ${dxc_SOURCE_DIR}/bin/x64/dxc.exe CACHE INTERNAL "")
  set(FLEDGE_DXC_DLL ${dxc_SOURCE_DIR}/bin/x64/dxcompiler.dll CACHE INTERNAL "")
  set(FLEDGE_DXC_DIR ${dxc_SOURCE_DIR} CACHE INTERNAL "")
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
  configure_file(${FLEDGE_DXC_DLL} ${CMAKE_BINARY_DIR}/bin/dxcompiler.dll COPYONLY)
endif()

# --- AMD FidelityFX Super Resolution 1 (MIT), shader headers ----------------
# ffx_a.h + ffx_fsr1.h are embedded as text into the HLSL of the fsr.easu / fsr.rcas passes.
FetchContent_Declare(fidelityfx_fsr
  URL https://github.com/GPUOpen-Effects/FidelityFX-FSR/archive/refs/tags/v1.0.2.tar.gz
  URL_HASH SHA256=92ee8f9630364ec3ad7fc16af5a990601ebf18142571527ed672b79b79d04acd
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_GetProperties(fidelityfx_fsr)
if(NOT fidelityfx_fsr_POPULATED)
  FetchContent_Populate(fidelityfx_fsr)
endif()
set(FLEDGE_FSR1_DIR ${fidelityfx_fsr_SOURCE_DIR}/ffx-fsr CACHE INTERNAL "")

# --- Intel XeSS SDK headers (Intel Simplified Software License) -------------
# Only the two C API headers are fetched (pinned by SHA-256) to compile the D3D12 XeSS bridge;
# libxess.dll itself is downloaded on demand by the user (licence shown first) or packaged.
if(WIN32)
  set(_xess_dir ${CMAKE_BINARY_DIR}/_deps/xess_headers/xess)
  set(_xess_ok TRUE)
  foreach(_f "xess.h|98dc435f5aed252d83a161fc1f86223f20c67eb863214fe3b5cb215a3f6e3319"
             "xess_d3d12.h|a582a60c3e39ee6fd6aa4871e23082cfb5ec510f98a19fc04acf7a7edb2f6b6c")
    string(REPLACE "|" ";" _p "${_f}")
    list(GET _p 0 _name)
    list(GET _p 1 _hash)
    if(NOT EXISTS ${_xess_dir}/${_name})
      file(DOWNLOAD https://raw.githubusercontent.com/intel/xess/v3.0.2/inc/xess/${_name} ${_xess_dir}/${_name} STATUS _st)
      list(GET _st 0 _code)
      if(NOT _code EQUAL 0)
        message(WARNING "XeSS header ${_name} could not be fetched (${_st}); XeSS support disabled")
        file(REMOVE ${_xess_dir}/${_name})
        set(_xess_ok FALSE)
      endif()
    endif()
    if(EXISTS ${_xess_dir}/${_name})
      file(SHA256 ${_xess_dir}/${_name} _actual)
      if(NOT _actual STREQUAL _hash)
        message(WARNING "XeSS header ${_name}: SHA-256 mismatch (${_actual}); XeSS support disabled")
        file(REMOVE ${_xess_dir}/${_name})
        set(_xess_ok FALSE)
      endif()
    endif()
  endforeach()
  if(_xess_ok)
    add_library(fledge_xess_headers INTERFACE)
    target_include_directories(fledge_xess_headers SYSTEM INTERFACE ${CMAKE_BINARY_DIR}/_deps/xess_headers)
    target_compile_definitions(fledge_xess_headers INTERFACE FLEDGE_HAVE_XESS=1)
  endif()
endif()
