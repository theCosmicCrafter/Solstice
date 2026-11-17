kis_add_tests(RestNoteCoreTest.cpp
    LINK_LIBRARIES kritarestnotecore kritatestsdk)

kis_add_test(RestNoteWidgetsTest.cpp ../RestNoteNotifications.cpp ../RestNoteSettingsDialog.cpp ../RestNoteDock.cpp
    TEST_NAME RestNoteWidgetsTest
    LINK_LIBRARIES kritarestnotecore kritaui kritatestsdk)
