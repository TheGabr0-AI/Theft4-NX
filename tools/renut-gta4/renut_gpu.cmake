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
    target_link_libraries(theft4_renut_gpu PRIVATE rex::runtime rexui)
    if(REXGLUE_PLATFORM_SWITCH)
        target_sources(theft4_renut_gpu PRIVATE "${engine}/gpu/banjo_renut_deko3d.cpp")
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
    target_link_libraries(${TARGET} PRIVATE theft4_renut_gpu)
    # The application's wiring is in its main.cpp. On that source only: a definition or an include directory on the
    # whole target would recompile every generated file (an hour at -j3).
    set_property(SOURCE main.cpp APPEND PROPERTY COMPILE_DEFINITIONS SA2_RENUT_RENDERER=1 THEFT4_RENUT_ENGINE=1)
    set_property(SOURCE main.cpp APPEND PROPERTY INCLUDE_DIRECTORIES "${THEFT4_RENUT_ROOT}")
endfunction()
