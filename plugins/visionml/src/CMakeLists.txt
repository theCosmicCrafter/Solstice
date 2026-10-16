set(kritavisionml_SOURCES
    VisionML.cpp
    VisionMLPlugin.cpp
    filters/BackgroundRemovalFilter.cpp
    inpaint/InpaintTool.cpp
    segmentation/SegmentationMatteUtils.h
    segmentation/SegmentationToolHelper.cpp
    segmentation/SelectSegmentFromPointTool.cpp
    segmentation/SelectSegmentFromRectTool.cpp
    smartselect/SmartMattingTool.cpp
    ../../tools/selectiontools/kis_selection_modifier_mapper.cc
)

qt_add_resources(kritavisionml_SOURCES ../icons/kritavisionml.qrc)

kis_add_library(kritavisionml MODULE ${kritavisionml_SOURCES})

generate_export_header(kritavisionml BASE_NAME kritavisionml)

target_include_directories(kritavisionml PRIVATE . ../../tools/selectiontools)
target_compile_features(kritavisionml PRIVATE cxx_std_20)
target_link_libraries(kritavisionml PRIVATE kritaui kritabasicflakes kritaimage visioncpp)

set_target_properties(kritavisionml PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN"
)

foreach(target ggml ggml-base ggml-cpu ggml-vulkan visioncpp)
    if(TARGET ${target})
        target_compile_options(${target} PRIVATE
            -Wno-cast-align -Wno-sign-compare -Wno-macro-redefined
            -Wno-unused-parameter -Wno-missing-prototypes -Wno-undef
            -Wno-unreachable-code)
    endif()
endforeach()

install(TARGETS kritavisionml DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install(FILES vision_tools.action vision_filters.action
        DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)

set(_vision_runtime_targets visioncpp ggml ggml-base ggml-cpu)
if(TARGET ggml-vulkan)
    list(APPEND _vision_runtime_targets ggml-vulkan)
endif()
if(WIN32)
    install(TARGETS ${_vision_runtime_targets}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
    install(TARGETS ${_vision_runtime_targets}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
