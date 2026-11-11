# Core — базовые типы; Document — модель данных, транзакции, отмена и повтор.
add_library(cosmocad_core core/src/Core.cpp)
target_include_directories(cosmocad_core PUBLIC core/include events/include)
target_link_libraries(cosmocad_core PRIVATE cosmocad_options)

add_library(cosmocad_document document/src/Document.cpp transactions/src/Transaction.cpp)
target_include_directories(cosmocad_document PUBLIC document/include transactions/include)
target_link_libraries(cosmocad_document PUBLIC cosmocad_core nlohmann_json::nlohmann_json PRIVATE cosmocad_options)
