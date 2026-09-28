find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2)

qt_add_executable(ghostty-shell main.cpp)
qt_add_qml_module(ghostty-shell
  URI GhosttyShell
  VERSION 1.0
  QML_FILES Main.qml
)
target_link_libraries(ghostty-shell PRIVATE Qt6::Quick Qt6::QuickControls2 qmlghosttyplugin)
