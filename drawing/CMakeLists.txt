# Drawing — объекты 2D-чертежа и сервис чертежа.
add_library(cosmocad_drawing src/Entities.cpp src/DrawingService.cpp src/DrawingEdit.cpp src/ModelViews.cpp)
target_include_directories(cosmocad_drawing PUBLIC include)
target_link_libraries(cosmocad_drawing PUBLIC cosmocad_document cosmocad_geometry PRIVATE cosmocad_options)
