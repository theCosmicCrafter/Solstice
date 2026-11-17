set(kritaspraypaintop_SOURCES
    spray_paintop_plugin.cpp
    kis_spray_paintop.cpp
    kis_spray_paintop_settings.cpp
    kis_spray_paintop_settings_widget.cpp
    spray_brush.cpp
    KisSprayRandomDistributions.cpp
    KisSprayOpOptionData.cpp
    KisSprayOpOptionModel.cpp
    KisSprayOpOptionWidget.cpp
    KisSprayOpOption.cpp
    
    KisSprayShapeOptionData.cpp
    KisSprayShapeOptionModel.cpp
    KisSprayShapeOptionWidget.cpp
    
    KisSprayShapeDynamicsOptionData.cpp
    KisSprayShapeDynamicsOptionModel.cpp
    KisSprayShapeDynamicsOptionWidget.cpp
    
    )

ki18n_wrap_ui(kritaspraypaintop_SOURCES wdgsprayoptions.ui wdgsprayshapeoptions.ui wdgshapedynamicsoptions.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritaspraypaintop_static_SOURCES ${kritaspraypaintop_SOURCES})
list(REMOVE_ITEM kritaspraypaintop_static_SOURCES spray_paintop_plugin.cpp)

kis_add_library(kritaspraypaintop_static STATIC ${kritaspraypaintop_static_SOURCES})
target_link_libraries(kritaspraypaintop_static kritaui kritalibpaintop)

kis_add_library(kritaspraypaintop MODULE spray_paintop_plugin.cpp)

target_link_libraries(kritaspraypaintop kritaspraypaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritaspraypaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})


########### install files ###############

install( FILES
    krita-spray.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)

