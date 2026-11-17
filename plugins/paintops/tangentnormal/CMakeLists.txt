set(kritatangentnormalpaintop_SOURCES
    kis_tangent_normal_paintop_plugin.cpp
    kis_tangent_normal_paintop.cpp
    kis_tangent_normal_paintop_settings_widget.cpp
    kis_normal_preview_widget.cpp
    KisTangentTiltOption.cpp
    KisTangentTiltOptionData.cpp
    KisTangentTiltOptionModel.cpp
    KisTangentTiltOptionWidget.cpp

)

ki18n_wrap_ui(kritatangentnormalpaintop_SOURCES wdgtangenttiltoption.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritatangentnormalpaintop_static_SOURCES ${kritatangentnormalpaintop_SOURCES})
list(REMOVE_ITEM kritatangentnormalpaintop_static_SOURCES kis_tangent_normal_paintop_plugin.cpp)

kis_add_library(kritatangentnormalpaintop_static STATIC ${kritatangentnormalpaintop_static_SOURCES})
target_link_libraries(kritatangentnormalpaintop_static kritaui kritalibpaintop)

kis_add_library(kritatangentnormalpaintop MODULE kis_tangent_normal_paintop_plugin.cpp)

target_link_libraries(kritatangentnormalpaintop kritatangentnormalpaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritatangentnormalpaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
########### install files ###############

install( FILES  krita-tangentnormal.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)
install( FILES  krita-tangentnormal-preview.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)

