set(solsticeeffects_SOURCES
    KisFilmGrainFilter.cpp
    KisFilmGrainConfigWidget.cpp
    KisChromaticAberrationFilter.cpp
    KisChromaticAberrationConfigWidget.cpp
    KisFilmFilter.cpp
    KisFilmConfigWidget.cpp
    KisCrystalFilter.cpp
    KisCrystalConfigWidget.cpp
    KisCrystalLattice.cpp
    KisPencilFilter.cpp
    KisPencilConfigWidget.cpp
    KisCpuPlaneBlur.cpp
    KisSharpnessFilter.cpp
    KisSharpnessConfigWidget.cpp
    KisColorFilter.cpp
    KisColorFilterConfigWidget.cpp
    KisGlowFilter.cpp
    KisGlowConfigWidget.cpp
    KisDropShadowFilter.cpp
    KisDropShadowConfigWidget.cpp
    KisLineArtFilter.cpp
    KisLineArtConfigWidget.cpp
    KisColorReferenceFilter.cpp
    KisColorReferenceResources.cpp
    KisColorReferenceConfigWidget.cpp
    KisRemoveDustFilter.cpp
    KisRemoveDustConfigWidget.cpp
)

kis_add_library(solsticeeffects STATIC ${solsticeeffects_SOURCES})
target_link_libraries(solsticeeffects PUBLIC kritaui)

kis_add_library(kritasolsticeeffectsfilter MODULE SolsticeEffectsPlugin.cpp)
target_link_libraries(kritasolsticeeffectsfilter solsticeeffects)
install(TARGETS kritasolsticeeffectsfilter DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
