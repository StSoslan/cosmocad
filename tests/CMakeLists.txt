find_package(GTest REQUIRED)
find_package(Python3 COMPONENTS Interpreter)
include(GoogleTest)

add_executable(cosmocad_tests
  unit/test_core_document.cpp
  unit/test_geometry_scene.cpp
  unit/test_standards.cpp
  unit/test_exchange_storage.cpp
  unit/test_kernel3d.cpp
  unit/test_modeling.cpp
  unit/test_assembly_app.cpp
  unit/test_schematic.cpp
  unit/test_libraries.cpp
  unit/test_sketcher.cpp
  unit/test_piping.cpp
  integration/test_interfaces_compile.cpp)
target_compile_definitions(cosmocad_tests PRIVATE COSMOCAD_TEST_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
# Тесты дополнений загружают пример дополнения из папки сборки.
target_compile_definitions(cosmocad_tests PRIVATE COSMOCAD_TEST_PLUGINS="${CMAKE_BINARY_DIR}/plugins")
add_dependencies(cosmocad_tests cosmocad_plugin_mass_report)
target_link_libraries(cosmocad_tests PRIVATE
  cosmocad_app cosmocad_exchange cosmocad_graphics cosmocad_modeling cosmocad_assembly cosmocad_geometry_occt
  GTest::gtest GTest::gtest_main cosmocad_options)
gtest_discover_tests(cosmocad_tests DISCOVERY_TIMEOUT 60)

# Сквозная проверка консольной утилиты: создать пример, проверить, экспортировать.
add_test(NAME cli_end_to_end
  COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:cosmocad-cli>
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_work
    -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/cli_end_to_end.cmake)

if(Python3_FOUND)
  # ОБ-4: в проекте нет зависимостей от ИИ.
  add_test(NAME no_ai_dependencies
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_no_ai.py ${CMAKE_SOURCE_DIR})
  # ОБ-5, ОБ-6: модули зависят друг от друга только в разрешённом направлении.
  add_test(NAME module_boundaries
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_module_boundaries.py ${CMAKE_SOURCE_DIR})
  # ОБ-11: скрипты на Python через модуль scripting/python/cosmocad.py и пакетный режим cosmocad-cli.
  add_test(NAME python_scripting
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripting/python/test_cosmocad.py)
  set_tests_properties(python_scripting PROPERTIES
    ENVIRONMENT "COSMOCAD_CLI=$<TARGET_FILE:cosmocad-cli>;COSMOCAD_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/python_user")
endif()

if(COSMOCAD_BUILD_UI)
  # Действия мышью и клавиатурой в настоящем окне.
  find_package(Qt6 REQUIRED COMPONENTS Test)
  set(CMAKE_AUTOMOC ON)
  add_executable(cosmocad_ui_tests ui/test_ui.cpp)
  target_compile_definitions(cosmocad_ui_tests PRIVATE COSMOCAD_TEST_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
  target_compile_definitions(cosmocad_ui_tests PRIVATE COSMOCAD_TEST_PLUGINS="${CMAKE_BINARY_DIR}/plugins")
  add_dependencies(cosmocad_ui_tests cosmocad_plugin_mass_report)
  target_link_libraries(cosmocad_ui_tests PRIVATE cosmocad_ui Qt6::Test cosmocad_options)
  add_test(NAME ui_interaction COMMAND cosmocad_ui_tests)
  set_tests_properties(ui_interaction PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

  # Окно открывается, рисует пример и сохраняет снимок (без экрана — платформа offscreen).
  add_test(NAME ui_smoke
    COMMAND cosmocad --smoke ${CMAKE_CURRENT_BINARY_DIR}/ui_smoke.png --pdf ${CMAKE_CURRENT_BINARY_DIR}/ui_smoke.pdf)
  set_tests_properties(ui_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    PASS_REGULAR_EXPRESSION "violations=0 screenshot=ok")
endif()

# Замер производительности (КАЧ-6). Запускается вручную, в ctest не входит.
add_executable(cosmocad_bench perf/bench.cpp)
target_compile_definitions(cosmocad_bench PRIVATE COSMOCAD_TEST_RESOURCES="${CMAKE_SOURCE_DIR}/resources")
target_link_libraries(cosmocad_bench PRIVATE cosmocad_app cosmocad_exchange cosmocad_graphics cosmocad_options)
