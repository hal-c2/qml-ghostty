# The `Ghostty` QML module: Terminal, the libghostty-vt surface. Static so an
# embedding app links it straight in; import it with `import Ghostty` after linking
# `qmlghosttyplugin` (or `qmlghostty` plus Q_IMPORT_QML_PLUGIN).
qt_add_qml_module(qmlghostty
  URI Ghostty
  VERSION 1.0
  STATIC
  SOURCES
    GhosttyTerminal.cpp GhosttyTerminal.h
    GhosttyKeys.cpp GhosttyKeys.h
)

target_include_directories(qmlghostty PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qmlghostty PUBLIC Qt6::Core Qt6::Gui Qt6::Quick PRIVATE ghostty::vt)
# libghostty-vt has struct fields named `emit`.
target_compile_definitions(qmlghostty PRIVATE QT_NO_EMIT)
