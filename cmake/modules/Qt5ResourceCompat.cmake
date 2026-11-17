# SPDX-License-Identifier: BSD-3-Clause
# Qt5 compatibility for the Qt6-style qt_add_resources() "big resource" form.
#
# Upstream sources call
#   qt_add_resources(<target> <name> PREFIX <p> BASE <dir> FILES <f...>)
# which only exists under Qt6. The Qt5CoreMacros.cmake wrapper in our build
# environment exposes qt_add_resources(<outvar> <input> ...) and misparses
# the big form (it treats the BASE directory as an input file). This module
# shadows that wrapper: calls containing PREFIX/BASE/FILES are translated to
# a generated .qrc in the current build dir (file alias = path relative to
# BASE, matching Qt6 semantics), while simple-form calls forward to
# qt5_add_resources() unchanged. Included only when QT_MAJOR_VERSION is 5.
if(QT_MAJOR_VERSION EQUAL 5 AND COMMAND qt5_add_resources)
function(qt_add_resources first second)
    set(_big FALSE)
    foreach(a IN LISTS ARGN)
        if(a STREQUAL "PREFIX" OR a STREQUAL "BASE" OR a STREQUAL "FILES")
            set(_big TRUE)
        endif()
    endforeach()
    if(NOT _big)
        # simple form: qt_add_resources(<outvar> <input>...)
        qt5_add_resources(${first} ${second} ${ARGN})
        set(${first} "${${first}}" PARENT_SCOPE)
        return()
    endif()
    # big form: qt_add_resources(<target> <name> PREFIX p BASE dir FILES ...)
    cmake_parse_arguments(QR "" "PREFIX;BASE" "FILES" ${ARGN})
    # Qt6 resolves relative BASE and FILES against the current source dir;
    # the generated .qrc lives in the build dir, so make them absolute.
    if(NOT QR_BASE)
        set(QR_BASE "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    get_filename_component(QR_BASE "${QR_BASE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_qrc "<RCC>\n<qresource prefix=\"${QR_PREFIX}\">\n")
    foreach(f IN LISTS QR_FILES)
        get_filename_component(f "${f}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        file(RELATIVE_PATH _rel "${QR_BASE}" "${f}")
        string(APPEND _qrc "    <file alias=\"${_rel}\">${f}</file>\n")
    endforeach()
    string(APPEND _qrc "</qresource>\n</RCC>\n")
    set(_qrcfile "${CMAKE_CURRENT_BINARY_DIR}/${second}.qrc")
    file(WRITE "${_qrcfile}" "${_qrc}")
    qt5_add_resources(_rcc_sources "${_qrcfile}")
    target_sources(${first} PRIVATE ${_rcc_sources})
endfunction()
endif()
