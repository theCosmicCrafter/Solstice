set(kritaexperimentpaintop_SOURCES
    experiment_paintop_plugin.cpp
    kis_experiment_paintop.cpp
    kis_experiment_paintop_settings.cpp
    kis_experiment_paintop_settings_widget.cpp
    KisExperimentOpOptionData.cpp
    KisExperimentOpOptionModel.cpp
    KisExperimentOpOptionWidget.cpp
    
    )

ki18n_wrap_ui(kritaexperimentpaintop_SOURCES wdgexperimentoptions.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritaexperimentpaintop_static_SOURCES ${kritaexperimentpaintop_SOURCES})
list(REMOVE_ITEM kritaexperimentpaintop_static_SOURCES experiment_paintop_plugin.cpp)

kis_add_library(kritaexperimentpaintop_static STATIC ${kritaexperimentpaintop_static_SOURCES})
target_link_libraries(kritaexperimentpaintop_static kritaui kritalibpaintop)

kis_add_library(kritaexperimentpaintop MODULE experiment_paintop_plugin.cpp)

target_link_libraries(kritaexperimentpaintop kritaexperimentpaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritaexperimentpaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES
    krita-experiment.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)


