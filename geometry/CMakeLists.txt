# Geometry — собственные геометрические типы и граница с 3D-ядром (IGeometryKernel).
add_library(cosmocad_geometry kernel/src/Geometry2D.cpp kernel/src/Geometry3D.cpp)
target_include_directories(cosmocad_geometry PUBLIC kernel/include)
target_link_libraries(cosmocad_geometry PUBLIC cosmocad_core PRIVATE cosmocad_options)

# Адаптер OCCT: единственное место, где подключаются заголовки OCCT.
find_package(OpenCASCADE REQUIRED)
add_library(cosmocad_geometry_occt adapters/occt/src/OcctKernel.cpp)
target_include_directories(cosmocad_geometry_occt PUBLIC adapters/occt/include)
target_include_directories(cosmocad_geometry_occt SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
# Имена библиотек обмена зависят от версии OpenCASCADE: до 7.8 — TKSTEP, TKSTEPBase, TKSTEPAttr,
# TKSTEP209 и TKSTL; начиная с 7.8 — TKDESTEP и TKDESTL. Берутся те, что есть в установленной версии.
set(COSMOCAD_OCCT_LIBS)
foreach(lib TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKFillet TKOffset
            TKShHealing TKMesh TKHLR TKXSBase TKSTEP TKSTEPBase TKSTEPAttr TKSTEP209 TKSTL TKDE TKDESTEP TKDESTL)
  if(TARGET ${lib})
    list(APPEND COSMOCAD_OCCT_LIBS ${lib})
  endif()
endforeach()
if(NOT TARGET TKSTEP AND NOT TARGET TKDESTEP)
  message(FATAL_ERROR "В установленной OpenCASCADE не найдена библиотека обмена STEP (TKSTEP или TKDESTEP)")
endif()
message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}: ${COSMOCAD_OCCT_LIBS}")
target_link_libraries(cosmocad_geometry_occt PUBLIC cosmocad_geometry PRIVATE cosmocad_options ${COSMOCAD_OCCT_LIBS})
