# Geometry — собственные геометрические типы и граница с 3D-ядром (IGeometryKernel).
add_library(cosmocad_geometry kernel/src/Geometry2D.cpp kernel/src/Geometry3D.cpp)
target_include_directories(cosmocad_geometry PUBLIC kernel/include)
target_link_libraries(cosmocad_geometry PUBLIC cosmocad_core PRIVATE cosmocad_options)

# Адаптер OCCT: единственное место, где подключаются заголовки OCCT.
find_package(OpenCASCADE REQUIRED)
add_library(cosmocad_geometry_occt adapters/occt/src/OcctKernel.cpp)
target_include_directories(cosmocad_geometry_occt PUBLIC adapters/occt/include)
target_include_directories(cosmocad_geometry_occt SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(cosmocad_geometry_occt PUBLIC cosmocad_geometry PRIVATE cosmocad_options
  TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKFillet TKOffset
  TKShHealing TKMesh TKHLR TKXSBase TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKSTL)
