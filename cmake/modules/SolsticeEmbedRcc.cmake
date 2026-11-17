# SPDX-FileCopyrightText: 2026 Solstice contributors
# SPDX-License-Identifier: BSD-3-Clause
#
# solstice_embed_rcc(<out_var> <symbol> <qrc file> DEPENDS <files...>)
#
# Compiles <qrc file> into a binary resource (rcc --binary) and embeds it as
# the 16-byte aligned array `const unsigned char <symbol>[]` in a generated
# source file, whose path is stored in <out_var>. The data can be mounted at
# any resource path at runtime with QResource::registerResource(data, mapRoot)
# (docs/agent/settings-location.md: KConfig names and embedded defaults).

set(SOLSTICE_EMBED_RCC_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/SolsticeEmbedRccToCpp.cmake")

function(solstice_embed_rcc out_var symbol qrc_file)
    cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    set(rcc_file "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.rcc")
    set(cpp_file "${CMAKE_CURRENT_BINARY_DIR}/${symbol}.cpp")
    add_custom_command(
        OUTPUT "${rcc_file}"
        COMMAND Qt${QT_MAJOR_VERSION}::rcc --binary -o "${rcc_file}" "${qrc_file}"
        DEPENDS "${qrc_file}" ${ARG_DEPENDS}
        COMMENT "Compiling binary resource ${symbol}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${cpp_file}"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${rcc_file}" "-DOUTPUT=${cpp_file}" "-DSYMBOL=${symbol}"
                -P "${SOLSTICE_EMBED_RCC_SCRIPT}"
        DEPENDS "${rcc_file}" "${SOLSTICE_EMBED_RCC_SCRIPT}"
        COMMENT "Embedding binary resource ${symbol}"
        VERBATIM)
    set(${out_var} "${cpp_file}" PARENT_SCOPE)
endfunction()
