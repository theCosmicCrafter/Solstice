# The MSIX packaging is only designed for x64

# Solstice: the version shows in the name while it has a label (alpha, beta)
if(SOLSTICE_VERSION_LABEL)
    set(MSIX_DISPLAY_NAME_SUFFIX " ${SOLSTICE_VERSION_STRING}")
    if(KRITA_GIT_SHA1_STRING)
        set(MSIX_DISPLAY_NAME_SUFFIX "${MSIX_DISPLAY_NAME_SUFFIX} (git ${KRITA_GIT_SHA1_STRING})")
    endif(KRITA_GIT_SHA1_STRING)
else()
    set(MSIX_DISPLAY_NAME_SUFFIX "")
endif()

configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/manifest.xml.in
    ${CMAKE_CURRENT_BINARY_DIR}/manifest.xml
    @ONLY
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/manifest.xml
        ${CMAKE_CURRENT_LIST_DIR}/build_msix.py
        ${CMAKE_CURRENT_LIST_DIR}/priconfig.xml
    DESTINATION
        ${CMAKE_INSTALL_PREFIX}/krita-msix
)

install(
    DIRECTORY 
        ${CMAKE_CURRENT_LIST_DIR}/pkg
    DESTINATION
        ${CMAKE_INSTALL_PREFIX}/krita-msix
)
