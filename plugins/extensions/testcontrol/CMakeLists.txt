set(kritatestcontrol_SOURCES
    TestControlPlugin.cpp
    TestControlHttpServer.cpp
    TestControlMcpServer.cpp
    TestControlInputInjector.cpp
    TestControlCanvasTools.cpp
    TestControlDocumentTools.cpp
    TestControlImageCompare.cpp
    TestControlPaths.cpp
    TestControlSupport.cpp
    TestControlTiming.cpp
    TestControlUiTools.cpp
)

kis_add_library(kritatestcontrol MODULE ${kritatestcontrol_SOURCES})

target_link_libraries(kritatestcontrol kritaui Qt${QT_MAJOR_VERSION}::Network)
# Only for the inline QSpontaneKeyEvent helper (qtestspontaneevent.h); the
# plugin does not link QtTest.
target_include_directories(kritatestcontrol PRIVATE
    $<TARGET_PROPERTY:Qt${QT_MAJOR_VERSION}::Test,INTERFACE_INCLUDE_DIRECTORIES>)

install(TARGETS kritatestcontrol DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
