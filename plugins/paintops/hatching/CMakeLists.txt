set(kritahatchingpaintop_SOURCES
    hatching_paintop_plugin.cpp
    kis_hatching_paintop.cpp
    kis_hatching_paintop_settings.cpp
    kis_hatching_paintop_settings_widget.cpp
    hatching_brush.cpp
    KisHatchingOptionsData.cpp
    KisHatchingOptionsModel.cpp
    KisHatchingOptionsWidget.cpp
    KisHatchingPreferencesData.cpp
    KisHatchingPreferencesModel.cpp
    KisHatchingPreferencesWidget.cpp
    )

ki18n_wrap_ui(kritahatchingpaintop_SOURCES wdghatchingpreferences.ui wdghatchingoptions.ui)

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritahatchingpaintop_static_SOURCES ${kritahatchingpaintop_SOURCES})
list(REMOVE_ITEM kritahatchingpaintop_static_SOURCES hatching_paintop_plugin.cpp)

kis_add_library(kritahatchingpaintop_static STATIC ${kritahatchingpaintop_static_SOURCES})
target_link_libraries(kritahatchingpaintop_static kritaui kritalibpaintop)

kis_add_library(kritahatchingpaintop MODULE hatching_paintop_plugin.cpp)

target_link_libraries(kritahatchingpaintop kritahatchingpaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritahatchingpaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES  krita-hatching.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)

