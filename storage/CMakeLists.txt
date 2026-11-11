# Storage — формат .cosmocad: сохранение, загрузка, миграции, восстановление.
add_library(cosmocad_storage src/ProjectStorage.cpp)
target_include_directories(cosmocad_storage PUBLIC include)
target_link_libraries(cosmocad_storage PUBLIC cosmocad_document nlohmann_json::nlohmann_json PRIVATE cosmocad_options)
