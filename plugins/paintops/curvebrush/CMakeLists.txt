set(kritacurvepaintop_SOURCES
      curve_paintop_plugin.cpp
      kis_curve_paintop.cpp
      kis_curve_paintop_settings.cpp
      kis_curve_paintop_settings_widget.cpp
      KisCurveOpOptionData.cpp
      KisCurveOpOptionModel.cpp
      KisCurveOpOptionWidget.cpp
      
    )

ki18n_wrap_ui(kritacurvepaintop_SOURCES wdgcurveoptions.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritacurvepaintop_static_SOURCES ${kritacurvepaintop_SOURCES})
list(REMOVE_ITEM kritacurvepaintop_static_SOURCES curve_paintop_plugin.cpp)

kis_add_library(kritacurvepaintop_static STATIC ${kritacurvepaintop_static_SOURCES})
target_link_libraries(kritacurvepaintop_static kritaui kritalibpaintop)

kis_add_library(kritacurvepaintop MODULE curve_paintop_plugin.cpp)

target_link_libraries(kritacurvepaintop kritacurvepaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritacurvepaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES  krita-curve.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)
