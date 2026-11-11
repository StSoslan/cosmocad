# Libraries — библиотека стандартных изделий (крепёж). Размеры — в данных, не в коде.
add_library(cosmocad_libraries src/Fasteners.cpp src/Materials.cpp src/Profiles.cpp)
target_include_directories(cosmocad_libraries PUBLIC include)
target_link_libraries(cosmocad_libraries PUBLIC cosmocad_modeling PRIVATE nlohmann_json::nlohmann_json cosmocad_options)
