configure_file(
    ${CMAKE_CURRENT_LIST_DIR}/MakeInstallerNsis.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MakeinstallerNsis.cmake
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/MakeinstallerNsis.cmake
    DESTINATION ${CMAKE_INSTALL_PREFIX}
)

install(FILES ${CMAKE_CURRENT_LIST_DIR}/installer_solstice.nsi
              ${CMAKE_CURRENT_LIST_DIR}/license_gpl-3.0.rtf
    DESTINATION ${CMAKE_INSTALL_PREFIX}/installer
)

install(FILES ${CMAKE_CURRENT_LIST_DIR}/include/FileExists2.nsh
              ${CMAKE_CURRENT_LIST_DIR}/include/IsFileInUse.nsh
    DESTINATION ${CMAKE_INSTALL_PREFIX}/installer/include
)
