# Дополнения, поставляемые вместе с программой. Каждое собирается в свою папку вместе с описью:
#   <сборка>/plugins/<имя>/plugin.json и библиотека.
function(cosmocad_add_plugin name directory)
  add_library(${name} MODULE ${ARGN})
  target_link_libraries(${name} PRIVATE cosmocad_app cosmocad_options)
  set_target_properties(${name} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/${directory}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/${directory})
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/${directory}/plugin.json ${CMAKE_BINARY_DIR}/plugins/${directory}/plugin.json COPYONLY)
  install(TARGETS ${name} LIBRARY DESTINATION share/cosmocad/plugins/${directory} RUNTIME DESTINATION share/cosmocad/plugins/${directory})
  install(FILES ${directory}/plugin.json DESTINATION share/cosmocad/plugins/${directory})
endfunction()

cosmocad_add_plugin(cosmocad_plugin_mass_report mass-report mass-report/MassReport.cpp)
