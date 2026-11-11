# Graphics — сцена, преобразование вида, выбор и привязки. Без Qt и без OpenGL.
add_library(cosmocad_graphics scene/src/Scene.cpp rendering/src/Render3D.cpp)
target_include_directories(cosmocad_graphics PUBLIC scene/include rendering/include)
target_link_libraries(cosmocad_graphics PUBLIC cosmocad_standards PRIVATE cosmocad_options)
