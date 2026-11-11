if (NOT TARGET fmt::fmt)
    add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}/fmt")
endif()

if (NOT TARGET xxHash::xxhash)
    add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}/xxHash/cmake_unofficial")
endif()

if (NOT TARGET libzstd)
    add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}/zstd/build/cmake")
endif()

# theft4 (2026-11-10): XENOS_RECOMP_DXC_BIN_ROOT names a dxc-bin folder with the binaries (bin/, lib/, inc/); the
# copy here has the headers only.
if (NOT TARGET Microsoft::DirectXShaderCompiler)
    if (XENOS_RECOMP_DXC_BIN_ROOT)
        add_subdirectory("${XENOS_RECOMP_DXC_BIN_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/dxc-bin")
    else()
        add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}/dxc-bin")
    endif()
endif()
