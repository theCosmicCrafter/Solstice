set(kritafilterop_SOURCES
    filterop.cpp
    kis_filterop.cpp
    kis_filterop_settings.cpp
    kis_filterop_settings_widget.cpp
    )


# Solstice: a static library for the tests of the settings widget
# (docs/agent/brush-option-shared-model.md)
set(kritafilterop_static_SOURCES ${kritafilterop_SOURCES})
list(REMOVE_ITEM kritafilterop_static_SOURCES filterop.cpp)

kis_add_library(kritafilterop_static STATIC ${kritafilterop_static_SOURCES})
target_link_libraries(kritafilterop_static kritaui kritalibpaintop)

kis_add_library(kritafilterop MODULE filterop.cpp)

target_link_libraries(kritafilterop kritafilterop_static)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

install(TARGETS kritafilterop  DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install( FILES
    krita-filterop.png
    DESTINATION ${KDE_INSTALL_DATADIR}/krita/images)
