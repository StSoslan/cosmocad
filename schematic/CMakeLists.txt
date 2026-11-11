# Schematic — электрические схемы: элементы, проводники, цепи, перечень элементов, проверка.
add_library(cosmocad_schematic src/Schematic.cpp)
target_include_directories(cosmocad_schematic PUBLIC include)
target_link_libraries(cosmocad_schematic PUBLIC cosmocad_drawing PRIVATE cosmocad_options)
