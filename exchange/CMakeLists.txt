# Exchange — импорт и экспорт. Поддержанные форматы: DXF (чтение и запись), SVG (запись).
add_library(cosmocad_exchange dxf/Dxf.cpp svg/Svg.cpp)
target_include_directories(cosmocad_exchange PUBLIC include)
target_link_libraries(cosmocad_exchange PUBLIC cosmocad_graphics PRIVATE cosmocad_options)
