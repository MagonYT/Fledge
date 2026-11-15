# Common compile options applied via fledge_configure_target().
function(fledge_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    if(WIN32)
      target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MINGW)
      target_compile_options(${target} PRIVATE -Wa,-mbig-obj)  # big translation units (component registry) exceed COFF's section limit
    endif()
  endif()
endfunction()

# Executables are linked fully static on MinGW so a packaged game does not
# need libstdc++/libgcc/winpthread DLLs next to it.
function(fledge_configure_executable target)
  fledge_configure_target(${target})
  if(MINGW)
    target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endfunction()

function(fledge_add_library name)
  file(GLOB_RECURSE _src CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
  add_library(${name} STATIC ${_src})
  target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                     PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  fledge_configure_target(${name})
endfunction()
