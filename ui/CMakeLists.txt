# UI — окна и виджеты на Qt 6. Единственный модуль, который знает о Qt.
set(CMAKE_AUTOMOC ON)
add_library(cosmocad_ui
  widgets/ScenePainter.cpp widgets/ScenePainter.h
  widgets/DrawingView.cpp widgets/DrawingView.h
  widgets/PropertiesPanel.cpp widgets/PropertiesPanel.h
  widgets/ModelView3D.cpp widgets/ModelView3D.h
  widgets/Ribbon.cpp widgets/Ribbon.h
  workspaces/MainWindow.cpp workspaces/MainWindow.h
  workspaces/ModelWindow.cpp workspaces/ModelWindow.h
  commands/Tool.h)
target_include_directories(cosmocad_ui PUBLIC widgets workspaces commands)
target_link_libraries(cosmocad_ui PUBLIC cosmocad_app cosmocad_exchange cosmocad_graphics Qt6::Widgets Qt6::PrintSupport PRIVATE cosmocad_options)

add_executable(cosmocad ../apps/cosmocad/main.cpp)
cosmocad_executable(cosmocad)
target_link_libraries(cosmocad PRIVATE cosmocad_ui cosmocad_options)
if(WIN32)
  set_target_properties(cosmocad PROPERTIES WIN32_EXECUTABLE ON)
endif()
install(TARGETS cosmocad RUNTIME DESTINATION bin)
