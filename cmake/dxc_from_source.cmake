# Use a DirectX Shader Compiler built from source instead of the prebuilt
# binaries in thirdparty/XenosRecomp/thirdparty/dxc-bin (a submodule that must
# not be initialised: the project builds only from source).
#
# Hooked in at configure time with
#   cmake --preset mac-arm64-native \
#         -DCMAKE_PROJECT_Fable2_INCLUDE=<abs path to this file> \
#         -DFABLE2_DXC_SOURCE=<DXC source tree> -DFABLE2_DXC_BUILD=<its build folder>
# XenosRecomp skips dxc-bin when Microsoft::DirectXShaderCompiler already
# exists, and cmake/host_shaders.cmake takes DIRECTX_DXC_TOOL.
# tools/mac/build_toolchains.sh builds DXC and prints these two folders;
# tools/mac/setup_mac.sh passes them.
#
# The folders can also come from the environment (FABLE2_DXC_SOURCE,
# FABLE2_DXC_BUILD); FABLE2_DXC_BUILD defaults to <source>/out/build.
#
# DXC v1.8.2407 (the version dxc-bin pins for macOS arm64), built with:
#   cmake -B out/build -G Ninja -C cmake/caches/PredefinedParams.cmake \
#     -DSPIRV_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
#     -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS=-Wno-invalid-specialization \
#     -DCLANG_FORMAT_EXE=CLANG_FORMAT_EXE-NOTFOUND
#   ninja -C out/build dxc dxcompiler
# plus one line commented out in its CMakeLists.txt (cmake_policy(SET CMP0051 OLD),
# which CMake 4 no longer accepts).
# It needs bin/dxc, lib/libdxcompiler.dylib and include/dxc/dxcapi.h.

set(FABLE2_DXC_SOURCE "" CACHE PATH "DirectXShaderCompiler source tree (v1.8.2407)")
if(NOT FABLE2_DXC_SOURCE AND NOT "$ENV{FABLE2_DXC_SOURCE}" STREQUAL "")
    set(FABLE2_DXC_SOURCE "$ENV{FABLE2_DXC_SOURCE}" CACHE PATH "" FORCE)
endif()
if(NOT FABLE2_DXC_SOURCE)
    message(FATAL_ERROR "dxc_from_source: set FABLE2_DXC_SOURCE (-D or environment) to a "
        "DirectXShaderCompiler v1.8.2407 source tree built from source; "
        "tools/mac/build_toolchains.sh builds one and prints it")
endif()
set(FABLE2_DXC_BUILD "" CACHE PATH "Its build folder (holds bin/dxc and lib/libdxcompiler.dylib)")
if(NOT FABLE2_DXC_BUILD)
    if(NOT "$ENV{FABLE2_DXC_BUILD}" STREQUAL "")
        set(FABLE2_DXC_BUILD "$ENV{FABLE2_DXC_BUILD}" CACHE PATH "" FORCE)
    else()
        set(FABLE2_DXC_BUILD "${FABLE2_DXC_SOURCE}/out/build" CACHE PATH "" FORCE)
    endif()
endif()

foreach(_f "${FABLE2_DXC_BUILD}/bin/dxc" "${FABLE2_DXC_BUILD}/lib/libdxcompiler.dylib"
           "${FABLE2_DXC_SOURCE}/include/dxc/dxcapi.h")
    if(NOT EXISTS "${_f}")
        message(FATAL_ERROR "dxc_from_source: missing ${_f}")
    endif()
endforeach()

if(NOT TARGET Microsoft::DirectXShaderCompiler)
    add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED GLOBAL)
    set_property(TARGET Microsoft::DirectXShaderCompiler PROPERTY
                 IMPORTED_LOCATION "${FABLE2_DXC_BUILD}/lib/libdxcompiler.dylib")
    target_include_directories(Microsoft::DirectXShaderCompiler INTERFACE
                               "${FABLE2_DXC_SOURCE}/include/dxc")
endif()
set(DIRECTX_DXC_TOOL "${FABLE2_DXC_BUILD}/bin/dxc" CACHE INTERNAL "")
message(STATUS "fable2: DXC built from source at ${FABLE2_DXC_BUILD}")
