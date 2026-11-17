include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../src/segmentation)

kis_add_test(VisionMLMatteTest.cpp
    TEST_NAME VisionMLMatteTest
    LINK_LIBRARIES Qt${QT_MAJOR_VERSION}::Core Qt${QT_MAJOR_VERSION}::Test kritaglobal kritatestsdk
)
