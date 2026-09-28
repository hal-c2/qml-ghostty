find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_terminal tst_terminal.cpp)
target_link_libraries(tst_terminal PRIVATE Qt6::Test Qt6::Quick qmlghosttyplugin)
add_test(NAME tst_terminal COMMAND tst_terminal)
set_tests_properties(tst_terminal PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
