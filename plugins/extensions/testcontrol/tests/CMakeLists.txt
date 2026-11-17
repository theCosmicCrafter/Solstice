kis_add_test(TestControlProtocolTest.cpp ../TestControlHttpServer.cpp ../TestControlMcpServer.cpp
    TEST_NAME TestControlProtocolTest
    LINK_LIBRARIES Qt${QT_MAJOR_VERSION}::Network kritatestsdk)
target_include_directories(TestControlProtocolTest PRIVATE ..)

kis_add_test(TestControlPathsTest.cpp ../TestControlPaths.cpp
    TEST_NAME TestControlPathsTest
    LINK_LIBRARIES kritatestsdk)
target_include_directories(TestControlPathsTest PRIVATE ..)

kis_add_test(TestControlImageCompareTest.cpp ../TestControlImageCompare.cpp
    TEST_NAME TestControlImageCompareTest
    LINK_LIBRARIES Qt${QT_MAJOR_VERSION}::Gui kritatestsdk)
target_include_directories(TestControlImageCompareTest PRIVATE ..)

kis_add_test(TestControlInputInjectorTest.cpp ../TestControlInputInjector.cpp ../TestControlTiming.cpp
    TEST_NAME TestControlInputInjectorTest
    LINK_LIBRARIES Qt${QT_MAJOR_VERSION}::Widgets kritatestsdk)
target_include_directories(TestControlInputInjectorTest PRIVATE ..)

kis_add_test(TestControlTimingTest.cpp ../TestControlTiming.cpp
    TEST_NAME TestControlTimingTest
    LINK_LIBRARIES Qt${QT_MAJOR_VERSION}::Widgets kritatestsdk)
target_include_directories(TestControlTimingTest PRIVATE ..)

# The scenario runner (dev-tools/test-control) against a fake endpoint.
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
    add_test(NAME plugins-extensions-testcontrol-RunScenariosTest
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/dev-tools/test-control/test_run_scenarios.py)
    add_test(NAME plugins-extensions-testcontrol-PaintTraceSeriesTest
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/dev-tools/test-control/test_paint_trace_series.py)
endif()
