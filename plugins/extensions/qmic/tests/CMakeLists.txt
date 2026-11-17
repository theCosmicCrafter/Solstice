kis_add_test(kis_qmic_tests.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../kis_qmic_simple_convertor.cpp
    TEST_NAME kis_qmic_tests
    LINK_LIBRARIES kritaimage kritaqmicinterface kritatestsdk
    )

kis_add_test(KisQmicIntegrationTest.cpp
    ../kis_qmic_import_tools.cpp ../kis_qmic_simple_convertor.cpp
    ../kis_qmic_synchronize_layers_command.cpp
    TEST_NAME KisQmicIntegrationTest
    LINK_LIBRARIES kritaqmicinterface kritaui kritatestsdk)

