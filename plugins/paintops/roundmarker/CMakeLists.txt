set(kritaroundmarkerpaintop_SOURCES
    roundmarker_paintop_plugin.cpp
    kis_roundmarkerop.cpp
    kis_roundmarkerop_settings.cpp
    kis_roundmarkerop_settings_widget.cpp
    KisRoundMarkerOpOptionData.cpp
    KisRoundMarkerOpOptionModel.cpp
    KisRoundMarkerOpOptionWidget.cpp
)

ki18n_wrap_ui(kritaroundmarkerpaintop_SOURCES kis_roundmarker_option.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritaroundmarkerpaintop_static_SOURCES ${kritaroundmarkerpaintop_SOURCES})
list(REMOVE_ITEM kritaroundmarkerpaintop_static_SOURCES roundmarker_paintop_plugin.cpp)

kis_add_library(kritaroundmarkerpaintop_static STATIC ${kritaroundmarkerpaintop_static_SOURCES})
target_link_libraries(kritaroundmarkerpaintop_static kritaui kritalibpaintop)

kis_add_library(kritaroundmarkerpaintop MODULE roundmarker_paintop_plugin.cpp)

target_link_libraries(kritaroundmarkerpaintop kritaroundmarkerpaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()


install(TARGETS kritaroundmarkerpaintop DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES  krita_roundmarkerop.svg DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)

