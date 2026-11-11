# Общая сборка модулей для приложений и консольная утилита.
add_library(cosmocad_app common/AppContext.cpp common/AppContext3D.cpp common/Batch.cpp common/Plugins.cpp)
target_link_libraries(cosmocad_app PUBLIC ${CMAKE_DL_LIBS})
target_include_directories(cosmocad_app PUBLIC common)
target_compile_definitions(cosmocad_app PRIVATE COSMOCAD_SOURCE_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
target_link_libraries(cosmocad_app PUBLIC cosmocad_storage cosmocad_standards cosmocad_drawing cosmocad_modeling cosmocad_assembly cosmocad_schematic cosmocad_libraries cosmocad_piping cosmocad_sketcher
  PRIVATE cosmocad_geometry_occt cosmocad_options)

add_executable(cosmocad-cli cosmocad-cli/main.cpp)
target_link_libraries(cosmocad-cli PRIVATE cosmocad_app cosmocad_exchange cosmocad_options)
install(TARGETS cosmocad-cli RUNTIME DESTINATION bin)
