add_subdirectory(tests)

set(kritacolorsmudgepaintop_SOURCES
    colorsmudge_paintop_plugin.cpp
    kis_colorsmudgeop.cpp
    kis_colorsmudgeop_settings.cpp
    kis_colorsmudgeop_settings_widget.cpp
    KisSmudgeLengthOptionData.cpp
    KisSmudgeLengthOptionModel.cpp
    KisSmudgeLengthOptionWidget.cpp
    KisSmudgeLengthOption.cpp
    KisPaintThicknessOptionData.cpp
    KisPaintThicknessOptionModel.cpp
    KisPaintThicknessOptionWidget.cpp
    KisPaintThicknessOption.cpp
    KisSmudgeOverlayModeOptionData.cpp
    KisSmudgeOverlayModeOptionModel.cpp
    KisSmudgeOverlayModeOptionWidget.cpp
    KisSmudgeRadiusOptionData.cpp
    KisGradientOption.cpp
    KisBrushPropertiesModel.cpp
    KisColorSmudgeStrategy.cpp
    KisColorSmudgeSource.cpp
    KisColorSmudgeStrategyBase.cpp
    KisColorSmudgeInterstrokeData.cpp
    KisColorSmudgeStrategyLightness.cpp
    KisColorSmudgeStrategyWithOverlay.cpp
    KisColorSmudgeStrategyMask.cpp
    KisColorSmudgeStrategyStamp.cpp
    KisColorSmudgeStrategyMaskLegacy.cpp)

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritacolorsmudgepaintop_static_SOURCES ${kritacolorsmudgepaintop_SOURCES})
list(REMOVE_ITEM kritacolorsmudgepaintop_static_SOURCES colorsmudge_paintop_plugin.cpp)

kis_add_library(kritacolorsmudgepaintop_static STATIC ${kritacolorsmudgepaintop_static_SOURCES})
target_link_libraries(kritacolorsmudgepaintop_static kritaui kritalibpaintop)

kis_add_library(kritacolorsmudgepaintop MODULE colorsmudge_paintop_plugin.cpp)

target_link_libraries(kritacolorsmudgepaintop kritacolorsmudgepaintop_static)

install(TARGETS kritacolorsmudgepaintop DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES  krita-colorsmudge.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)

