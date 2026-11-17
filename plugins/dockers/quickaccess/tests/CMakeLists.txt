kis_add_tests(
    QuickAccessCoreTest.cpp
    LINK_LIBRARIES kritaquickaccesscore kritatestsdk
)

kis_add_test(QuickAdjustKeyControllerTest.cpp ../QuickAdjustKeyController.cpp
    TEST_NAME QuickAdjustKeyControllerTest
    LINK_LIBRARIES kritaui kritatestsdk
)
target_include_directories(QuickAdjustKeyControllerTest PRIVATE ..)

kis_add_tests(QuickAccessConfigTest.cpp
    LINK_LIBRARIES kritaquickaccessconfig kritatestsdk)

kis_add_test(QuickAccessItemPropertiesTest.cpp ../QuickAccessItemProperties.cpp ../QuickAccessSettingsDialog.cpp
    TEST_NAME QuickAccessItemPropertiesTest
    LINK_LIBRARIES kritaquickaccessconfig Qt${QT_MAJOR_VERSION}::Widgets kritatestsdk)
qt_add_resources(QuickAccessItemPropertiesTest quickaccess_test_icons
    PREFIX "/quickaccess" BASE "../resources" FILES "../resources/default_icons/reset.png")

kis_add_test(QuickAccessGridEditTest.cpp
    ../QuickAccessGridEditDialog.cpp ../QuickAccessGridCanvas.cpp
    ../QuickAccessGridItemButton.cpp ../QuickAccessStrokePreviews.cpp
    TEST_NAME QuickAccessGridEditTest
    LINK_LIBRARIES kritaquickaccesscore kritaui kritatestsdk)
qt_add_resources(QuickAccessGridEditTest quickaccess_grid_test_icons
    PREFIX "/quickaccess" BASE "../resources" FILES "../resources/default_icons/reset.png")

kis_add_test(QuickAdjustDockTest.cpp ../QuickAdjustDock.cpp ../QuickAdjustDockHistory.cpp
    ../QuickAdjustToolOptionsPad.cpp ../QuickAdjustKeyController.cpp
    TEST_NAME QuickAdjustDockTest
    LINK_LIBRARIES kritaui kritatestsdk)
target_include_directories(QuickAdjustDockTest PRIVATE ..)
# the status icons whose updates the test follows
qt_add_resources(QuickAdjustDockTest quickaccess_adjust_test_icons
    PREFIX "/quickaccess" BASE "../resources" FILES
    "../resources/quick_adjust/gesture_on.png" "../resources/quick_adjust/gesture_off.png"
    "../resources/quick_adjust/erase_mode_on.png" "../resources/quick_adjust/erase_mode_off.png")

# Compile the actual palette integration, excluding only factory registration.
set(quickaccess_interaction_sources ${KRITA_QUICKACCESS_DOCKER_SOURCES})
list(REMOVE_ITEM quickaccess_interaction_sources QuickAccessDockerPlugin.cpp)
list(TRANSFORM quickaccess_interaction_sources PREPEND "../")
kis_add_test(QuickAccessInteractionTest.cpp ${quickaccess_interaction_sources}
    TEST_NAME QuickAccessInteractionTest
    LINK_LIBRARIES kritaquickaccessconfig kritaui kritatestsdk)
target_include_directories(QuickAccessInteractionTest PRIVATE ..)
if(PYTHON_LIBRARY)
    target_include_directories(QuickAccessInteractionTest SYSTEM PRIVATE "${PYTHON_INCLUDE_DIRS}")
    target_link_libraries(QuickAccessInteractionTest ${PYTHON_LIBRARY})
    target_compile_definitions(QuickAccessInteractionTest PRIVATE HAVE_QUICKACCESS_PYTHON=1)
endif()
qt_add_resources(QuickAccessInteractionTest quickaccess_interaction_test_icons
    PREFIX "/quickaccess" BASE "../resources" FILES ${KRITA_QUICKACCESS_ICON_FILES})
