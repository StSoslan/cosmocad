# Standards — профили стандартов и проверка оформления.
# Не зависит от интерфейса пользователя и от 3D-ядра.
add_library(cosmocad_standards engine/src/Profile.cpp engine/src/Engine.cpp)
target_include_directories(cosmocad_standards PUBLIC engine/include)
target_link_libraries(cosmocad_standards PUBLIC cosmocad_drawing nlohmann_json::nlohmann_json PRIVATE cosmocad_options)
