# Modeling — параметрическая 3D-модель детали: формулы, эскизы, операции, перестроение.
# Зависит от геометрического ядра только через интерфейс IGeometryKernel.
add_library(cosmocad_modeling
  expressions/src/Expression.cpp
  features/src/PartObjects.cpp
  recompute/src/ModelingService.cpp)
target_include_directories(cosmocad_modeling PUBLIC expressions/include features/include recompute/include)
target_link_libraries(cosmocad_modeling PUBLIC cosmocad_document cosmocad_geometry PRIVATE cosmocad_options)
