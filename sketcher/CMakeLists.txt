# Sketcher — ограничения для линий, окружностей и дуг чертежа и численный решатель.
add_library(cosmocad_sketcher src/Sketcher.cpp)
target_include_directories(cosmocad_sketcher PUBLIC include)
target_link_libraries(cosmocad_sketcher PUBLIC cosmocad_drawing PRIVATE cosmocad_options)
