cmake_minimum_required(VERSION 3.24)
project(CosmoCad VERSION 0.1.0 LANGUAGES CXX)

# CosmoCad — универсальная инженерная CAD-система.
# В проекте нет и не должно быть зависимостей от ИИ (см. scripts/check_no_ai.py).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(COSMOCAD_BUILD_UI "Собирать оконное приложение (нужен Qt 6)" ON)
option(COSMOCAD_BUILD_TESTS "Собирать тесты (нужен GoogleTest)" ON)
option(COSMOCAD_WARNINGS_AS_ERRORS "Считать предупреждения компилятора ошибками" OFF)

find_package(nlohmann_json 3.11 REQUIRED)

add_library(cosmocad_options INTERFACE)
if(MSVC)
  target_compile_options(cosmocad_options INTERFACE /W4 /permissive- /utf-8)
  if(COSMOCAD_WARNINGS_AS_ERRORS)
    target_compile_options(cosmocad_options INTERFACE /WX)
  endif()
else()
  target_compile_options(cosmocad_options INTERFACE -Wall -Wextra -Wpedantic)
  if(COSMOCAD_WARNINGS_AS_ERRORS)
    target_compile_options(cosmocad_options INTERFACE -Werror)
  endif()
endif()

# Порядок каталогов повторяет направление зависимостей: нижние не знают о верхних.
add_subdirectory(core)
add_subdirectory(geometry)
add_subdirectory(drawing)
add_subdirectory(standards)
add_subdirectory(graphics)
add_subdirectory(exchange)
add_subdirectory(storage)
add_subdirectory(modeling)
add_subdirectory(assembly)
add_subdirectory(schematic)
add_subdirectory(libraries)
add_subdirectory(piping)
add_subdirectory(sketcher)
add_subdirectory(apps)
add_subdirectory(plugins)

if(COSMOCAD_BUILD_UI)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets PrintSupport)
  add_subdirectory(ui)
endif()

if(COSMOCAD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(DIRECTORY resources/standards resources/libraries DESTINATION share/cosmocad/resources)
install(FILES scripting/python/cosmocad.py DESTINATION share/cosmocad/python)
install(DIRECTORY scripting/python/examples DESTINATION share/cosmocad/python)
