set(kritaparticlepaintop_SOURCES
    particle_paintop_plugin.cpp
    kis_particle_paintop.cpp
    kis_particle_paintop_settings.cpp
    kis_particle_paintop_settings_widget.cpp
    particle_brush.cpp
    KisParticleOpOptionData.cpp
    KisParticleOpOptionModel.cpp
    KisParticleOpOptionWidget.cpp
    )

ki18n_wrap_ui(kritaparticlepaintop_SOURCES wdgparticleoptions.ui )

# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritaparticlepaintop_static_SOURCES ${kritaparticlepaintop_SOURCES})
list(REMOVE_ITEM kritaparticlepaintop_static_SOURCES particle_paintop_plugin.cpp)

kis_add_library(kritaparticlepaintop_static STATIC ${kritaparticlepaintop_static_SOURCES})
target_link_libraries(kritaparticlepaintop_static kritaui kritalibpaintop)

kis_add_library(kritaparticlepaintop MODULE particle_paintop_plugin.cpp)

target_link_libraries(kritaparticlepaintop kritaparticlepaintop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritaparticlepaintop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})


########### install files ###############

install( FILES  krita-particle.png DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)


