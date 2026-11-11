# ReNut's Direct3D-level renderer for GTA IV (docs/switch-port/18-renut-migration.md, step R2), chosen at boot with
# gta4_renderer = "renut". From sa2-nx's app/cmake/sa2_renut_gpu.cmake: Renut's gpu.cmake with its dependencies where
# this repository has them (thirdparty/renut: renut_engine, plume, reblue-XenosRecomp, imgui).
#
#   include(tools/renut-gta4/renut_gpu.cmake)
#   theft4_renut_gpu(<executable> <generated code dir>)
#
# An OBJECT library, not a static one: its hooks replace the generated code's weak sub_X definitions, and a static
# archive's members are only pulled in for undefined symbols, so the hooks would silently not be linked.
#
# Local inputs, never in the repository (game data):
#   THEFT4_RENUT_SHADER_DUMP_DIR  the folder of GTA IV's shader containers (*.bin: tools/deko3d/gta4_fxc_shaders.py
#                                 extract). The translator builds the shader cache from them into the build tree.

get_filename_component(THEFT4_RENUT_GTA4_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(THEFT4_RENUT_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(THEFT4_RENUT_ROOT "${THEFT4_RENUT_REPO}/thirdparty/renut")

set(THEFT4_RENUT_SHADER_DUMP_DIR "" CACHE PATH
    "Folder of GTA IV's shader containers (*.bin), made locally by tools/deko3d/gta4_fxc_shaders.py extract")
if(NOT REXGLUE_PLATFORM_SWITCH)
    if(NOT EXISTS "${THEFT4_RENUT_SHADER_DUMP_DIR}")
        message(FATAL_ERROR "THEFT4_RENUT: set THEFT4_RENUT_SHADER_DUMP_DIR to the folder of GTA IV's shader "
                            "containers (tools/deko3d/gta4_fxc_shaders.py extract --out <folder>)")
    endif()
endif()
# Normal variables: they shadow gpu.cmake's cache entries.
set(RENUT_SHADER_DUMP_DIR "${THEFT4_RENUT_SHADER_DUMP_DIR}")
set(RENUT_ASSETS_DIR "${THEFT4_RENUT_SHADER_DUMP_DIR}")
# The DXC binaries (thirdparty/renut has the headers only): the copy of tools/XenosRecomp.
set(RENUT_DXC_BIN_ROOT "${THEFT4_RENUT_REPO}/tools/XenosRecomp/thirdparty/dxc-bin" CACHE PATH
    "A dxc-bin folder with bin/, lib/ and inc/")
if(NOT EXISTS "${RENUT_DXC_BIN_ROOT}/bin/x64/dxc.exe" AND CMAKE_HOST_WIN32)
    message(FATAL_ERROR "THEFT4_RENUT: no DXC in ${RENUT_DXC_BIN_ROOT} (git submodule update --init tools/XenosRecomp)")
endif()
# banjo-nx's early alpha pass needs banjo-nx's tools (tools/renut_early_alpha.py), which are not here: one translator
# run. It is one of the shader passes of 18 section 4 (class 2: to measure on GTA IV's shaders first).
set(BANJO_RENUT_EARLY_ALPHA OFF CACHE BOOL "Early alpha copies in Renut's pixel shaders (banjo-nx's tool: not here)" FORCE)

# Renut's XenosRecomp is built as an ExternalProject (gpu.cmake): it gets this build's whole host toolchain, not only
# the compiler paths. On the PC, clang for the MSVC ABI.
# The translator's libraries without their command-line programs (thirdparty/renut keeps their library sources only).
set(BANJO_RENUT_XENOS_RECOMP_ARGS -DXXHASH_BUILD_XXHSUM=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF
                                  -DZSTD_BUILD_SHARED=OFF)
foreach(_theft4_var CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET CMAKE_LINKER_TYPE CMAKE_AR CMAKE_RC_COMPILER
                    CMAKE_MT CMAKE_MAKE_PROGRAM)
    if(DEFINED ${_theft4_var} AND NOT "${${_theft4_var}}" STREQUAL "")
        list(APPEND BANJO_RENUT_XENOS_RECOMP_ARGS "-D${_theft4_var}=${${_theft4_var}}")
    endif()
endforeach()

# Renut's SDK exports o1heap as rex::o1heap.
if(TARGET o1heap AND NOT TARGET rex::o1heap)
    add_library(rex::o1heap ALIAS o1heap)
endif()

if(NOT Python3_EXECUTABLE)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
if(NOT PYTHON_EXECUTABLE)
    set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
endif()

# Step R5 (18-renut-migration.md): the Switch build, a cross build on the Windows host, as sa2-nx's
# app/cmake/sa2_renut_gpu.cmake makes it: plume's deko3d back end, no Vulkan driver (REXGLUE_SWITCH_NO_NVK), uam for the
# shader variants on the console. The host tools run on the PC: the host shaders' deko3d blobs (DXC, SPIRV-Cross, uam
# built for the PC: tools/deko3d/deko3d_build_tools.sh) at build time; the game shaders are made beforehand from the
# PC build's translator (tools/renut-gta4/renut_switch_shaders.ps1) into THEFT4_RENUT_SWITCH_SHADERS_DIR (game data).
if(REXGLUE_PLATFORM_SWITCH)
    set(PLUME_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
    set(PLUME_SWITCH_DEVKITPRO "${DEVKITPRO}" CACHE PATH "" FORCE)
    # plume checks that a driver archive exists even when it links none: the portlibs' NVK, never linked here.
    set(PLUME_SWITCH_NVK_ROOT "${DEVKITPRO}/portlibs/switch" CACHE PATH "" FORCE)
    set(PLUME_SWITCH_NVK_LIBRARY "${DEVKITPRO}/portlibs/switch/lib/libvulkan.a" CACHE FILEPATH "" FORCE)
    set(PLUME_SWITCH_NO_NVK ON CACHE BOOL "" FORCE)
    # This repository's uam (the old deko3d renderer's, SwitchWakerHD's patched 1.1.0 with banjo-nx's passes).
    set(PLUME_SWITCH_UAM_DIR "${THEFT4_RENUT_REPO}/thirdparty/uam" CACHE PATH "" FORCE)
    set(PLUME_DEKO3D ON CACHE BOOL "deko3d back end (Switch)" FORCE)
    set(BANJO_DEKO3D_HOST_SHADERS ON)
    set(BANJO_DEKO3D_TOOLS_DIR "${THEFT4_RENUT_REPO}/tools/deko3d")
    set(BANJO_DEKO3D_SPIRV_CROSS "${THEFT4_RENUT_REPO}/out/deko3d_tools/spirv-cross-build/spirv-cross.exe" CACHE FILEPATH
        "SPIRV-Cross for the deko3d host shaders (tools/deko3d/deko3d_build_tools.sh)")
    set(BANJO_DEKO3D_UAM "${THEFT4_RENUT_REPO}/out/deko3d_tools/uam-build/uam.exe" CACHE FILEPATH
        "uam built for the PC, for the deko3d host shaders (tools/deko3d/deko3d_build_tools.sh)")
    foreach(_theft4_tool BANJO_DEKO3D_SPIRV_CROSS BANJO_DEKO3D_UAM)
        if(NOT EXISTS "${${_theft4_tool}}")
            message(FATAL_ERROR "No ${${_theft4_tool}}: run tools/deko3d/deko3d_build_tools.sh first")
        endif()
    endforeach()
    set(THEFT4_RENUT_SWITCH_SHADERS_DIR "${THEFT4_RENUT_REPO}/out/renut-switch" CACHE PATH
        "shader_cache_switch.cpp and deko3d_glsl_switch.cpp (tools/renut-gta4/renut_switch_shaders.ps1; game data)")
    set(THEFT4_RENUT_SHADER_CACHE_CPP "${THEFT4_RENUT_SWITCH_SHADERS_DIR}/shader_cache_switch.cpp")
    set(THEFT4_RENUT_GLSL_CPP "${THEFT4_RENUT_SWITCH_SHADERS_DIR}/deko3d_glsl_switch.cpp")
    foreach(_theft4_input THEFT4_RENUT_SHADER_CACHE_CPP THEFT4_RENUT_GLSL_CPP)
        if(NOT EXISTS "${${_theft4_input}}")
            message(FATAL_ERROR "No ${${_theft4_input}}: run tools/renut-gta4/renut_switch_shaders.ps1 (needs the PC "
                                "build's translator, tools/pc-eu-base with THEFT4_RENUT_ENGINE)")
        endif()
    endforeach()
    # No dump and no translator run in this build: gpu.cmake only checks that the folder exists.
    set(RENUT_SHADER_DUMP_DIR "${THEFT4_RENUT_SWITCH_SHADERS_DIR}")
    set(RENUT_ASSETS_DIR "${THEFT4_RENUT_SWITCH_SHADERS_DIR}")
    # gpu.cmake's step 3 with the PC build's table (renut_switch_shader_cache.py: no DXIL, no SPIR-V).
    function(banjo_renut_shader_cache_override TARGET)
        target_sources(${TARGET} PRIVATE "${THEFT4_RENUT_SHADER_CACHE_CPP}")
        set_source_files_properties("${THEFT4_RENUT_SHADER_CACHE_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    endfunction()
endif()

include("${THEFT4_RENUT_ROOT}/renut_engine/gpu/gpu.cmake")
# The SDK links its own volk on the PC: plume's copy under private names (plume_vulkan.h).
if(TARGET plume AND NOT REXGLUE_PLATFORM_SWITCH)
    target_compile_definitions(plume PUBLIC PLUME_VOLK_PRIVATE_NAMES=1)
endif()

function(theft4_renut_gpu TARGET GENERATED_DIR)
    set(engine "${THEFT4_RENUT_ROOT}/renut_engine")
    add_library(theft4_renut_gpu OBJECT
        "${engine}/gpu/banjo_renut_select.cpp"
        "${engine}/gpu/banjo_renut_tuning.cpp"
        "${engine}/gpu/banjo_renut_checks.cpp"
        "${engine}/gpu/banjo_renut_record.cpp"
        "${engine}/display_sync.cpp"
        "${engine}/perf/frame_stats.cpp"
        # sa2-nx: the frame latency probe (from futari-nx) and the store cards switched off, app-side.
        "${engine}/sa2_compat/frame_latency.cpp"
        "${engine}/sa2_compat/sa2_compat.cpp"
        # sa2-nx: the game's frames at its presentation interval (the console's vertical blank).
        "${engine}/gpu/sa2_renut_pacing.cpp"
        # GTA IV: what the renderer asks of the application around it (compat/).
        "${THEFT4_RENUT_GTA4_DIR}/gta4_renut_compat.cpp")
    target_include_directories(theft4_renut_gpu PRIVATE
        "${THEFT4_RENUT_ROOT}"
        "${THEFT4_RENUT_GTA4_DIR}"
        # The SA2 application's headers the renderer includes, answered for GTA IV; ahead of sa2_compat.
        "${THEFT4_RENUT_GTA4_DIR}/compat"
        "${engine}/sa2_compat"
        "${GENERATED_DIR}"
        "${THEFT4_SDK_DIR}/src/core")
    if(DEFINED REXGLUE_ENTRYPOINT_INCLUDE_DIR)
        target_include_directories(theft4_renut_gpu PRIVATE "${REXGLUE_ENTRYPOINT_INCLUDE_DIR}")
    endif()
    if(REXGLUE_PLATFORM_SWITCH)
        # The SDK builds no rexui for a Switch build without Vulkan: the application's THEFT4_RENUT_UI_LIBRARY (its
        # window context, the immediate and ImGui drawers, Dear ImGui) instead.
        target_link_libraries(theft4_renut_gpu PRIVATE rex::runtime ${THEFT4_RENUT_UI_LIBRARY})
        target_sources(theft4_renut_gpu PRIVATE "${engine}/gpu/banjo_renut_deko3d.cpp" "${THEFT4_RENUT_GLSL_CPP}")
        set_source_files_properties("${THEFT4_RENUT_GLSL_CPP}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    else()
        target_link_libraries(theft4_renut_gpu PRIVATE rex::runtime rexui)
    endif()
    if(COMMAND rexglue_apply_target_settings)
        rexglue_apply_target_settings(theft4_renut_gpu)
    endif()
    renut_gpu_setup(theft4_renut_gpu)
    # SA2_RENUT_RENDERER: sa2-nx's guard around its own additions to the engine. THEFT4_RENUT: this port's.
    target_compile_definitions(theft4_renut_gpu PRIVATE SA2_RENUT_RENDERER=1 THEFT4_RENUT=1 THEFT4_RENUT_ENGINE=1)
    # The generated code's rules (tools/pc-eu-base: the hooks read and write guest state as it does).
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(theft4_renut_gpu PRIVATE -ffp-contract=off -fno-strict-aliasing)
    endif()
    if(REXGLUE_PLATFORM_SWITCH)
        # sa2-nx: no Vulkan driver in this build, so Renut on deko3d whatever gta4_renderer / renut_gpu_backend say (per
        # source: nothing else recompiles). Line tables for the console's crash reports and profiles (-g1, as sa2-nx).
        set_property(SOURCE "${engine}/gpu/banjo_renut_select.cpp" "${RENUT_GPU_DIR}/backend/backend.cpp"
                     TARGET_DIRECTORY theft4_renut_gpu APPEND PROPERTY COMPILE_DEFINITIONS SA2_SWITCH_NO_NVK=1)
        target_compile_options(theft4_renut_gpu PRIVATE -g1)
        # The SDK's headers need C++23 (std::byteswap); on the PC the SDK's target settings give it.
        target_compile_features(theft4_renut_gpu PRIVATE cxx_std_23)
        # The engine's Switch code tests REX_PLATFORM_SWITCH (sa2-nx's SDK name); this SDK's platform.h names it
        # REX_PLATFORM_NX and defines both only inside its own folder.
        target_compile_definitions(theft4_renut_gpu PRIVATE REX_PLATFORM_SWITCH=1)
    endif()
    target_link_libraries(${TARGET} PRIVATE theft4_renut_gpu)
    # The application's wiring is in its main.cpp. On that source only: a definition or an include directory on the
    # whole target would recompile every generated file (an hour at -j3).
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS SA2_RENUT_RENDERER=1 THEFT4_RENUT_ENGINE=1)
    set_property(SOURCE main.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${THEFT4_RENUT_ROOT}")
endfunction()
