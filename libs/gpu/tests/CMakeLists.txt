kis_add_tests(
    KisGpuEngineTest.cpp
    LINK_LIBRARIES kritagpu kritapigment kritatestsdk Qt${QT_MAJOR_VERSION}::Concurrent
    )

# testPreciseContraction: the probe optimized (as the engine's shaders) and
# not optimized, in float and double
set(precise_probe_SHADERS)
kis_gpu_add_shader(precise_probe_SHADERS precise_probe_float precise_probe.comp)
kis_gpu_add_shader(precise_probe_SHADERS precise_probe_double precise_probe.comp DEFINES PROBE_DOUBLE)
kis_gpu_add_shader(precise_probe_SHADERS precise_probe_half precise_probe.comp DEFINES PROBE_HALF)
foreach(probe float double)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/shaders/precise_probe_${probe}_unoptimized.spv.inc)
    set(defines)
    if(probe STREQUAL "double")
        set(defines -DPROBE_DOUBLE)
    endif()
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -mfmt=num ${defines}
                -o ${output} ${CMAKE_CURRENT_SOURCE_DIR}/precise_probe.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/precise_probe.comp
        COMMENT "Compiling GPU probe shader ${probe} (unoptimized)"
        VERBATIM)
    list(APPEND precise_probe_SHADERS ${output})
endforeach()
# testBlendProbe: composite_blend.glsl on its own, RGBA32F and RGBA16F
foreach(variant rgba32f rgba16f)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/shaders/blend_probe_${variant}.spv.inc)
    set(defines)
    if(variant STREQUAL "rgba16f")
        set(defines -DTILE_F16)
    endif()
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} --target-env=vulkan1.3 -O -mfmt=num ${defines}
                -I ${CMAKE_CURRENT_SOURCE_DIR}/../shaders
                -o ${output} ${CMAKE_CURRENT_SOURCE_DIR}/blend_probe.comp
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/blend_probe.comp
                ${CMAKE_CURRENT_SOURCE_DIR}/../shaders/composite_blend.glsl
                ${CMAKE_CURRENT_SOURCE_DIR}/../shaders/composite_half_brush.glsl
        COMMENT "Compiling GPU blend probe ${variant}"
        VERBATIM)
    list(APPEND precise_probe_SHADERS ${output})
endforeach()
target_sources(KisGpuEngineTest PRIVATE ${precise_probe_SHADERS})
target_include_directories(KisGpuEngineTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)

if(WIN32)
    kis_add_tests(
        KisGpuGLInteropTest.cpp
        LINK_LIBRARIES kritagpu kritatestsdk Qt${QT_MAJOR_VERSION}::OpenGL
        )
endif()
