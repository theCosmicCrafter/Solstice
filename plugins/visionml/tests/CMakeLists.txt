include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../src/segmentation)

kis_add_test(VisionMLMatteTest.cpp
    TEST_NAME VisionMLMatteTest
    LINK_LIBRARIES Qt6::Core Qt6::Test kritatestsdk
)
