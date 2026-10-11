# GPU engine (Solstice): Vulkan compute foundation.
# See docs/agent/gpu-engine.md. The library is optional: without the Vulkan
# SDK headers and glslc it is not built and nothing else depends on it yet.

find_package(Vulkan QUIET COMPONENTS glslc)
if(NOT Vulkan_INCLUDE_DIRS OR NOT Vulkan_GLSLC_EXECUTABLE)
    message(STATUS "Solstice GPU engine: Vulkan headers/glslc not found; kritagpu is not built")
    set(HAVE_KRITA_GPU_ENGINE OFF CACHE INTERNAL "kritagpu is built")
    return()
endif()
set(HAVE_KRITA_GPU_ENGINE ON CACHE INTERNAL "kritagpu is built")

# kis_gpu_add_shader(<output-var> <name> <source> [DEFINES ...] [INCLUDES ...])
# Compiles a GLSL compute shader to SPIR-V as a comma-separated list of words
# (<name>.spv.inc) that sources include inside an array initializer.
function(kis_gpu_add_shader output_var name source)
    cmake_parse_arguments(ARG "" "" "DEFINES;INCLUDES" ${ARGN})
    set(defines)
    foreach(define IN LISTS ARG_DEFINES)
        list(APPEND defines "-D${define}")
    endforeach()
    set(output ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv.inc)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num ${defines}
                -o ${output} ${CMAKE_CURRENT_SOURCE_DIR}/${source}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${source} ${ARG_INCLUDES}
        COMMENT "Compiling GPU shader ${name}"
        VERBATIM)
    set(${output_var} ${${output_var}} ${output} PARENT_SCOPE)
endfunction()

set(kritagpu_SHADERS)
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs shaders/paint_dabs.comp DEFINES BASIC_BLEND_ONLY
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl)
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_extended shaders/paint_dabs.comp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl)
# Textured dabs (phase 4.102): separate RGBA32F builds, so that the texture
# code does not change how the drivers compile the untextured variants.
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_textured shaders/paint_dabs.comp DEFINES BASIC_BLEND_ONLY DAB_TEXTURE
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl)
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_textured_extended shaders/paint_dabs.comp DEFINES DAB_TEXTURE
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl)
# RGBA16F image tips (phase 4.105), separate builds like the RGBA32F ones
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_textured_rgba16f shaders/paint_dabs.comp
    DEFINES TILE_F16 BASIC_BLEND_ONLY DAB_TEXTURE
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_half_brush.glsl)
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_textured_extended_rgba16f shaders/paint_dabs.comp DEFINES TILE_F16 DAB_TEXTURE
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_half_brush.glsl)
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_rgba16f shaders/paint_dabs.comp DEFINES TILE_F16 BASIC_BLEND_ONLY
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_half_brush.glsl)
kis_gpu_add_shader(kritagpu_SHADERS paint_dabs_extended_rgba16f shaders/paint_dabs.comp DEFINES TILE_F16
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_half_brush.glsl)
kis_gpu_add_shader(kritagpu_SHADERS composite_over_stack_rgba32f shaders/composite_over_stack.comp)
kis_gpu_add_shader(kritagpu_SHADERS composite_over_stack_rgba16f shaders/composite_over_stack.comp DEFINES TILE_F16)
kis_gpu_add_shader(kritagpu_SHADERS fill_tiles_rgba32f shaders/fill_tiles.comp)
kis_gpu_add_shader(kritagpu_SHADERS fill_tiles_rgba16f shaders/fill_tiles.comp DEFINES TILE_F16)
kis_gpu_add_shader(kritagpu_SHADERS composite_layers_rgba32f shaders/composite_layers.comp DEFINES BASIC_BLEND_ONLY
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl)
kis_gpu_add_shader(kritagpu_SHADERS composite_layers_rgba16f shaders/composite_layers.comp DEFINES TILE_F16 BASIC_BLEND_ONLY
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_half_brush.glsl)
kis_gpu_add_shader(kritagpu_SHADERS composite_layers_extended_rgba32f shaders/composite_layers.comp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl)
kis_gpu_add_shader(kritagpu_SHADERS composite_layers_extended_rgba16f shaders/composite_layers.comp DEFINES TILE_F16
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_blend.glsl
             ${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite_half_brush.glsl)
kis_gpu_add_shader(kritagpu_SHADERS canvas_patches_f32_f32 shaders/canvas_patches.comp)
kis_gpu_add_shader(kritagpu_SHADERS canvas_patches_f32_f16 shaders/canvas_patches.comp DEFINES DST_F16)
kis_gpu_add_shader(kritagpu_SHADERS canvas_patches_f16_f32 shaders/canvas_patches.comp DEFINES SRC_F16)
kis_gpu_add_shader(kritagpu_SHADERS canvas_patches_f16_f16 shaders/canvas_patches.comp DEFINES SRC_F16 DST_F16)
kis_gpu_add_shader(kritagpu_SHADERS transform_pass_rgba32f shaders/transform_pass.comp)
kis_gpu_add_shader(kritagpu_SHADERS transform_pass_rgba16f shaders/transform_pass.comp DEFINES TILE_F16)
kis_gpu_add_shader(kritagpu_SHADERS grid_warp_claim shaders/grid_warp.comp DEFINES CLAIM)
kis_gpu_add_shader(kritagpu_SHADERS grid_warp_resolve_rgba32f shaders/grid_warp.comp)
kis_gpu_add_shader(kritagpu_SHADERS grid_warp_resolve_rgba16f shaders/grid_warp.comp DEFINES TILE_F16)
kis_gpu_add_shader(kritagpu_SHADERS separable_convolution_h_rgba32f shaders/separable_convolution.comp)
kis_gpu_add_shader(kritagpu_SHADERS separable_convolution_h_rgba16f shaders/separable_convolution.comp DEFINES TILE_F16)
kis_gpu_add_shader(kritagpu_SHADERS separable_convolution_v_rgba32f shaders/separable_convolution.comp DEFINES VERTICAL)
kis_gpu_add_shader(kritagpu_SHADERS separable_convolution_v_rgba16f shaders/separable_convolution.comp DEFINES VERTICAL TILE_F16)
# Solstice: the masking brush composite (docs/agent/gpu-engine.md)
kis_gpu_add_shader(kritagpu_SHADERS masking_composite_rgba32f shaders/masking_composite.comp)

set(kritagpu_LIB_SRCS
    KisGpuVulkanFunctions.cpp
    KisGpuContext.cpp
    KisGpuBuffer.cpp
    KisGpuCommandList.cpp
    KisGpuComputePipeline.cpp
    KisGpuTilePool.cpp
    KisGpuLayerStackCompositor.cpp
    KisGpuTileFill.cpp
    KisGpuDabCompositor.cpp
    KisGpuLayerCompositor.cpp
    KisGpuCanvasPatchWriter.cpp
    KisGpuTransformPass.cpp
    KisGpuGridWarpPass.cpp
    KisGpuSeparableConvolutionPass.cpp
    KisGpuMaskingCompositePass.cpp
)

if(WIN32)
    list(APPEND kritagpu_LIB_SRCS KisGpuGLSharedImage.cpp KisGpuGLSharedBuffer.cpp)
endif()

kis_add_library(kritagpu SHARED ${kritagpu_LIB_SRCS} ${kritagpu_SHADERS})
generate_export_header(kritagpu BASE_NAME kritagpu)

target_include_directories(kritagpu
    PUBLIC
        ${Vulkan_INCLUDE_DIRS}
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/shaders
)

# The Vulkan loader is resolved at runtime (KisGpuVulkanFunctions), so
# kritagpu does not link against vulkan-1.
target_link_libraries(kritagpu
    PUBLIC
        kritaglobal
        Qt${QT_MAJOR_VERSION}::Core
        Qt${QT_MAJOR_VERSION}::Gui
)

set_target_properties(kritagpu PROPERTIES
    VERSION ${GENERIC_KRITA_LIB_VERSION} SOVERSION ${GENERIC_KRITA_LIB_SOVERSION}
)
install(TARGETS kritagpu ${INSTALL_TARGETS_DEFAULT_ARGS})

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
