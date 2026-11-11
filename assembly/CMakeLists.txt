# Assembly — компоненты, сопряжения, проверка пересечений, состав изделия.
add_library(cosmocad_assembly src/AssemblyService.cpp)
target_include_directories(cosmocad_assembly PUBLIC include)
target_link_libraries(cosmocad_assembly PUBLIC cosmocad_document cosmocad_geometry PRIVATE cosmocad_options)
