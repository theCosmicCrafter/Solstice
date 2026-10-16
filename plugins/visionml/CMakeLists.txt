# Native AI-powered selection, background removal, and smart fill tools.

find_package(Vulkan QUIET COMPONENTS glslc)
if(Vulkan_FOUND)
    set(VISP_VULKAN ON CACHE BOOL "Enable the Vision ML Vulkan backend" FORCE)
else()
    set(VISP_VULKAN OFF CACHE BOOL "Enable the Vision ML Vulkan backend" FORCE)
    message(STATUS "Krita Vision: Vulkan SDK/glslc not found; building the CPU backend only")
endif()
set(VISP_CI ON CACHE BOOL "Build portable Vision ML backends" FORCE)
set(VISP_TESTS OFF CACHE BOOL "Do not build vision.cpp tests" FORCE)
set(VISP_INSTALL_MODELS OFF CACHE BOOL "Krita installs its own model layout" FORCE)
set(VISP_BUILD_CLI OFF CACHE BOOL "Do not build the standalone vision.cpp CLI" FORCE)
set(VISP_ENABLE_INSTALL OFF CACHE BOOL "Do not install standalone vision.cpp files" FORCE)

if(UNIX)
    set(VISP_FMT_LIB ON CACHE BOOL "Use fmt where std::format is unavailable" FORCE)
endif()

add_subdirectory(thirdparty/vision.cpp)
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

set(_vision_model_root "${CMAKE_CURRENT_BINARY_DIR}/models")

function(krita_vision_download_model relative_path url sha256)
    set(output "${_vision_model_root}/${relative_path}")
    get_filename_component(output_dir "${output}" DIRECTORY)
    get_filename_component(relative_dir "${relative_path}" DIRECTORY)
    file(MAKE_DIRECTORY "${output_dir}")
    if(NOT EXISTS "${output}")
        message(STATUS "Downloading Krita Vision model ${relative_path}")
        file(DOWNLOAD "${url}" "${output}"
             EXPECTED_HASH "SHA256=${sha256}"
             SHOW_PROGRESS)
    endif()
    install(FILES "${output}"
            DESTINATION "${KDE_INSTALL_DATADIR}/krita/visionml/models/${relative_dir}")
endfunction()

krita_vision_download_model(
    "sam/MobileSAM-F16.gguf"
    "https://huggingface.co/Acly/MobileSAM-GGUF/resolve/main/MobileSAM-F16.gguf"
    "b546366475e3ad744bb2eaf7634df88e9aaf25f6622797d2de300f5a530831f7")
krita_vision_download_model(
    "birefnet/BiRefNet-lite-F16.gguf"
    "https://huggingface.co/Acly/BiRefNet-GGUF/resolve/main/BiRefNet-lite-F16.gguf"
    "7b5397a2c98d66677f8f74317774bbeac49dbb321b8a3dc744af913db71d4fa5")
krita_vision_download_model(
    "migan/MIGAN-512-places2-F16.gguf"
    "https://huggingface.co/Acly/MIGAN-GGUF/resolve/main/MIGAN-512-places2-F16.gguf"
    "3e47592bf716d0dc306f8dc02d4476cfcdaf2c055fa3c3c8e0ced4db775eb64b")

install(FILES LICENSE
        DESTINATION "${KDE_INSTALL_DATADIR}/krita/visionml/licenses"
        RENAME "krita-vision-tools-LICENSE")
install(FILES thirdparty/vision.cpp/LICENSE
        DESTINATION "${KDE_INSTALL_DATADIR}/krita/visionml/licenses"
        RENAME "vision.cpp-LICENSE")

if(WIN32)
    find_file(VISIONML_LIBOMP NAMES libomp.dll REQUIRED)
    install(FILES "${VISIONML_LIBOMP}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
