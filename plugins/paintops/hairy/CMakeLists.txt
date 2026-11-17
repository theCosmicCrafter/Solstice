set(kritahairypaintop_SOURCES
    hairy_paintop_plugin.cpp
    kis_hairy_paintop.cpp
    kis_hairy_paintop_settings.cpp
    kis_hairy_paintop_settings_widget.cpp
    bristle.cpp
    hairy_brush.cpp
    trajectory.cpp
    KisHairyBristleOptionData.cpp
    KisHairyBristleOptionModel.cpp
    KisHairyBristleOptionWidget.cpp
    KisHairyInkOptionData.cpp
    KisHairyInkOptionModel.cpp
    KisHairyInkOptionWidget.cpp
    )

ki18n_wrap_ui(kritahairypaintop_SOURCES wdgInkOptions.ui  wdghairyshapeoptions.ui wdgbristleoptions.ui)

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritahairypaintop_static_SOURCES ${kritahairypaintop_SOURCES})
list(REMOVE_ITEM kritahairypaintop_static_SOURCES hairy_paintop_plugin.cpp)

kis_add_library(kritahairypaintop_static STATIC ${kritahairypaintop_static_SOURCES})
target_link_libraries(kritahairypaintop_static kritaui kritalibpaintop)

kis_add_library(kritahairypaintop MODULE hairy_paintop_plugin.cpp)

target_link_libraries(kritahairypaintop kritahairypaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritahairypaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES
    krita-sumi.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)


