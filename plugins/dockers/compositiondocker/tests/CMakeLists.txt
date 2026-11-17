kis_add_test(CompositionWorkflowTest.cpp ../compositionmodel.cpp ../CompositionDocumentState.cpp
    TEST_NAME CompositionWorkflowTest
    LINK_LIBRARIES kritaui kritalibkra kritatestsdk
)
target_include_directories(CompositionWorkflowTest PRIVATE ..
    ${CMAKE_SOURCE_DIR}/plugins/impex/libkra ${CMAKE_BINARY_DIR}/plugins/impex/libkra)
