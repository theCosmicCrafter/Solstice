set(kritagridpaintop_SOURCES
    grid_paintop_plugin.cpp
    kis_grid_paintop.cpp
    KisGridOpOptionData.cpp
    KisGridOpOptionModel.cpp
    KisGridOpOptionWidget.cpp
    
    kis_grid_paintop_settings.cpp
    kis_grid_paintop_settings_widget.cpp
    KisGridShapeOptionData.cpp
    KisGridShapeOptionModel.cpp
    KisGridShapeOptionWidget.cpp
    )

ki18n_wrap_ui(kritagridpaintop_SOURCES wdggridoptions.ui wdggridbrushshapeoptions.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritagridpaintop_static_SOURCES ${kritagridpaintop_SOURCES})
list(REMOVE_ITEM kritagridpaintop_static_SOURCES grid_paintop_plugin.cpp)

kis_add_library(kritagridpaintop_static STATIC ${kritagridpaintop_static_SOURCES})
target_link_libraries(kritagridpaintop_static kritaui kritalibpaintop)

kis_add_library(kritagridpaintop MODULE grid_paintop_plugin.cpp)

target_link_libraries(kritagridpaintop kritagridpaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritagridpaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES
    krita-grid.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)


