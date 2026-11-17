include_directories(.. ${CMAKE_CURRENT_BINARY_DIR}/..)
kis_add_test(KisLensBlurFilterTest.cpp
    ../kis_lens_blur_filter.cpp ../kis_wdg_lens_blur.cpp ../KisLensBlurHighlights.cpp
    TEST_NAME KisLensBlurFilterTest LINK_LIBRARIES kritaui kritatestsdk)
add_dependencies(KisLensBlurFilterTest kritablurfilter)
