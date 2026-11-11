# Piping — напорные трубы из полипропилена: прочность, допустимое давление, срок службы.
# Коэффициенты материалов и классы эксплуатации — в данных, не в коде.
add_library(cosmocad_piping src/Pipes.cpp)
target_include_directories(cosmocad_piping PUBLIC include)
target_link_libraries(cosmocad_piping PUBLIC cosmocad_modeling PRIVATE nlohmann_json::nlohmann_json cosmocad_options)
