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
# The native D3D12 renderer: ReXNative (https://github.com/gtguerreiro-max/ReXNative), the
# universal version of PGR4Native's renderer this port started from (DLAA / DLSS / DLSS 5, FSR,
# the settings HUD). It hooks the guest D3D API and draws without EDRAM emulation; runs under
# Proton on the Deck. Everything specific to From Dust is in src/rexnative/game_layer.h and
# d3d_surface.inc, written by ReXNative's tool (python -m rexnative adapt, with
# src/rexnative/overrides.json). Windows only.
option(FROMDUST_NATIVE "Build the native D3D12 renderer (ReXNative)" ${WIN32})
if(WIN32 AND FROMDUST_NATIVE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ReXNative/renderer/rexnative.cmake")
        set(_fromdust_rexnative "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ReXNative")
    else()
        set(_fromdust_rexnative "${CMAKE_CURRENT_SOURCE_DIR}/../ReXNative")
    endif()
    set(FROMDUST_REXNATIVE_DIR "${_fromdust_rexnative}" CACHE PATH
        "ReXNative (the third_party/ReXNative submodule, or a checkout beside this one)")
    set(FROMDUST_REXGLUE_SDK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/build/rexglue-sdk" CACHE PATH
        "The source tree the ReXGlue SDK was built from")
    set(FROMDUST_FIDELITYFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/build/fidelityfx-sdk2" CACHE PATH
        "AMD FidelityFX SDK v2.3.0 (60f4ea8, as PGR4Native's scripts/fidelityfx.lock)")
    set(FROMDUST_DLSS_SDK "F:/PGR4Native/third_party/dlss_sdk" CACHE PATH "NVIDIA DLSS SDK")
    set(FROMDUST_DLSS_NR "F:/PGR4Native/third_party/dlss_nr" CACHE PATH "NVIDIA DLSS 5 headers")
    include("${FROMDUST_REXNATIVE_DIR}/renderer/rexnative.cmake")
    # From Dust's own side of the renderer: its world stepping (frame_rate.cpp) and the other
    # game-side functions (native_glue.cpp).
    rexnative_add(fromdust
        LAYER_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src/rexnative"
        REXGLUE_SDK_SRC "${FROMDUST_REXGLUE_SDK_SRC}"
        FIDELITYFX_SDK  "${FROMDUST_FIDELITYFX_SDK}"
        DLSS_SDK        "${FROMDUST_DLSS_SDK}"
        DLSS_NR         "${FROMDUST_DLSS_NR}"
        NO_DEFAULT_GLUE)
    target_sources(fromdust PRIVATE src/native_glue.cpp)
    target_compile_definitions(fromdust PRIVATE FROMDUST_NATIVE_RENDERER=1)
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
