# fromdust - ReXGlue port of From Dust (XBLA, title 58410A91), on ReXGlue v0.11.

cmake_minimum_required(VERSION 3.25)
project(fromdust LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(rexglue 0.11.0 REQUIRED CONFIG)
# The game and the six XEX images it loads with XexLoadImage (Galileo*, dustai), each a module
# (generated/<module>, built as a DLL beside the executable by generated/default/dll_targets.cmake).
rexglue_project(fromdust OUTPUT_DIR generated/default)

# Host code: the app, the world's stepping and the water's flow (src/), and ReXNative below.
# OBJECT, not STATIC: an archive drops the hooks.
add_library(fromdust_common OBJECT src/fromdust_app.cpp src/frame_rate.cpp src/water_flow.cpp)
rexglue_setup_host_library(fromdust_common)

if(WIN32)
    add_executable(fromdust WIN32 src/main.cpp)
else()
    add_executable(fromdust src/main.cpp)
endif()

# ReXGlue's Xenos GPU drivers (Xenia's D3D12 backend; vkd3d-proton turns it into Vulkan on the
# Deck).
rexglue_setup_target(fromdust)
target_link_libraries(fromdust PRIVATE fromdust_common)

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
    set(FROMDUST_FIDELITYFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/build/fidelityfx-sdk" CACHE PATH
        "AMD FidelityFX SDK v2.4.0 (7539ae8, ReXNative's sdk/fidelityfx.lock)")
    set(FROMDUST_DLSS_SDK "F:/PGR4Native/third_party/dlss_sdk" CACHE PATH "NVIDIA DLSS SDK")
    set(FROMDUST_DLSS_NR "F:/PGR4Native/third_party/dlss_nr" CACHE PATH "NVIDIA DLSS 5 headers")
    include("${FROMDUST_REXNATIVE_DIR}/renderer/rexnative.cmake")
    # From Dust's own side of the renderer: its world stepping (frame_rate.cpp) and the other
    # game-side functions (native_glue.cpp).
    rexnative_add(fromdust_common
        EXECUTABLE      fromdust
        LAYER_DIR       "${CMAKE_CURRENT_SOURCE_DIR}/src/rexnative"
        REXGLUE_SDK_SRC "${FROMDUST_REXGLUE_SDK_SRC}"
        FIDELITYFX_SDK  "${FROMDUST_FIDELITYFX_SDK}"
        DLSS_SDK        "${FROMDUST_DLSS_SDK}"
        DLSS_NR         "${FROMDUST_DLSS_NR}"
        NO_DEFAULT_GLUE)
    target_sources(fromdust_common PRIVATE src/native_glue.cpp)
    target_compile_definitions(fromdust_common PRIVATE FROMDUST_NATIVE_RENDERER=1
        # frame_rate.cpp hooks XInputGetState itself (and calls the HUD's FilterPad): ReXNative's
        # own hook of it stays out.
        REXNATIVE_GAME_HOOKS_XINPUT=1)
endif()

# Link maps: they name the recompiled guest functions of a crash's offsets.
target_link_options(fromdust PRIVATE "LINKER:/MAP")
foreach(_module GalileoKernel GalileoPathfind_9_9 GalileoSimulatorWater
        GalileoSimulatorWater_9_9 GalileoTexture dustai_x360_f)
    if(TARGET fromdust_${_module})
        target_link_options(fromdust_${_module} PRIVATE "LINKER:/MAP")
    endif()
endforeach()
