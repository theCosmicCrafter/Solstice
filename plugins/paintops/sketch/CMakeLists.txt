set(kritasketchpaintop_SOURCES
    sketch_paintop_plugin.cpp
    kis_sketch_paintop.cpp
    kis_sketch_paintop_settings.cpp
    kis_sketch_paintop_settings_widget.cpp
    KisSketchOpOptionData.cpp
    KisSketchOpOptionModel.cpp
    KisSketchOpOptionWidget.cpp
    )

ki18n_wrap_ui(kritasketchpaintop_SOURCES wdgsketchoptions.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritasketchpaintop_static_SOURCES ${kritasketchpaintop_SOURCES})
list(REMOVE_ITEM kritasketchpaintop_static_SOURCES sketch_paintop_plugin.cpp)

kis_add_library(kritasketchpaintop_static STATIC ${kritasketchpaintop_static_SOURCES})
target_link_libraries(kritasketchpaintop_static kritaui kritalibpaintop)

kis_add_library(kritasketchpaintop MODULE sketch_paintop_plugin.cpp)

target_link_libraries(kritasketchpaintop kritasketchpaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritasketchpaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})


########### install files ###############

install( FILES  krita-sketch.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)


