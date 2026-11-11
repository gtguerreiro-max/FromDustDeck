# fromdust - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fromdust LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FROMDUST_SOURCES
    src/main.cpp
    src/crash_report.cpp
    src/frame_rate.cpp
    src/water_flow.cpp
    src/guest_capture.cpp
)

if(WIN32)
    add_executable(fromdust WIN32 ${FROMDUST_SOURCES})
else()
    add_executable(fromdust ${FROMDUST_SOURCES})
endif()

# The Xenos GPU plugin (Xenia's D3D12 backend; vkd3d-proton turns it into Vulkan on the Deck).
rexglue_setup_target(fromdust GPU_PLUGINS xenos)
# The native D3D12 renderer (src/native, adapted from PGR4Native: it hooks the guest D3D API and
# draws without EDRAM emulation; runs under Proton on the Deck). Windows only.
option(FROMDUST_NATIVE "Build the native D3D12 renderer (src/native)" ${WIN32})
if(WIN32 AND FROMDUST_NATIVE)
    set(FROMDUST_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/build/rexglue-sdk" CACHE PATH
        "The source tree the ReXGlue SDK was built from")
    set(FROMDUST_FIDELITYFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/build/fidelityfx-sdk2" CACHE PATH
        "AMD FidelityFX SDK v2.3.0 (60f4ea8, as PGR4Native's scripts/fidelityfx.lock)")
    set(FROMDUST_DLSS_SDK "F:/PGR4Native/third_party/dlss_sdk" CACHE PATH "NVIDIA DLSS SDK")
    set(FROMDUST_DLSS_NR "F:/PGR4Native/third_party/dlss_nr" CACHE PATH "NVIDIA DLSS 5 headers")
    file(GLOB FROMDUST_NATIVE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/native/*.cpp")
    target_sources(fromdust PRIVATE
        src/fromdust_d3d_trace.cpp
        src/fromdust_settings.cpp
        src/native_glue.cpp
        ${FROMDUST_NATIVE_SOURCES}
        # Texture format tables the renderer shares with the Xenos plugin, which exports none.
        "${FROMDUST_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/util.cpp"
        "${FROMDUST_REXGLUE_SDK_SRC}/src/graphics/pipeline/texture/info_formats.cpp")
    # FSR 1 for the present (src/native/fsr.cpp): the SDK's prebuilt presenter shaders.
    target_include_directories(fromdust PRIVATE src
        "${FROMDUST_REXGLUE_SDK_SRC}/src/ui/shaders/bytecode")
    target_compile_definitions(fromdust PRIVATE FROMDUST_NATIVE_RENDERER=1)
    target_include_directories(fromdust SYSTEM PRIVATE
        "${FROMDUST_FIDELITYFX_SDK}/Kits/FidelityFX/api/include"
        "${FROMDUST_FIDELITYFX_SDK}/Kits/FidelityFX/upscalers/include"
        "${FROMDUST_FIDELITYFX_SDK}/Kits/FidelityFX/framegeneration/include"
        "${FROMDUST_DLSS_SDK}/include" "${FROMDUST_DLSS_NR}/include")
    target_link_libraries(fromdust PRIVATE d3d12 dxgi dxguid delayimp
        "${FROMDUST_DLSS_SDK}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
    target_link_options(fromdust PRIVATE /DELAYLOAD:d3d12.dll)
    set(PGR4_REXGLUE_SDK_SRC "${FROMDUST_REXGLUE_SDK_SRC}")
    include(src/native/shader_xlate.cmake)
    target_link_libraries(fromdust PRIVATE pgr4_shader_xlate)
endif()

# Link maps for crash.txt's module+offset frames (src/crash_report.cpp).
if(MSVC)
    target_link_options(fromdust PRIVATE /MAP)
    foreach(_module GalileoKernel GalileoPathfind_9_9 GalileoSimulatorWater
            GalileoSimulatorWater_9_9 GalileoTexture dustai_x360_f)
        if(TARGET fromdust_${_module})
            target_link_options(fromdust_${_module} PRIVATE /MAP)
        endif()
    endforeach()
endif()
