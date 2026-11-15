add_executable(fledge_editor
  main.cpp editor_app.cpp editor_theme.cpp editor_commands.cpp editor_shell.cpp editor_ai.cpp
  editor_viewport.cpp editor_panels.cpp editor_assets.cpp editor_scripts.cpp editor_graphics.cpp
  editor_code.cpp editor_simple.cpp editor_innovations.cpp editor_world.cpp editor_animation.cpp editor_physics.cpp editor_audio.cpp editor_gameplay.cpp editor_store.cpp editor_blueprint.cpp editor_edition.cpp editor_autosave.cpp editor_migration.cpp editor_mcp_control.cpp editor_app.h editor_icons.h editor_code.h)
target_link_libraries(fledge_editor PRIVATE fledge_editor_core ${FLEDGE_EDITION_LIB} imgui)
if(WIN32)
  target_link_libraries(fledge_editor PRIVATE comdlg32)  # Asset Store Import file picker
endif()
fledge_configure_executable(fledge_editor)
# GUI subsystem: no console window. stdout still works when redirected (ctest, scripts) - see attachParentConsoleIfNeeded().
set_target_properties(fledge_editor PROPERTIES WIN32_EXECUTABLE ON OUTPUT_NAME Fledge)
fledge_add_version_resource(fledge_editor "Fledge Editor")
add_dependencies(fledge_editor fledge_runtime)
