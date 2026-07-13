# SPDX-License-Identifier: BSD-3-Clause
cmake_minimum_required(VERSION 3.24)
project(polymesh VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Never resolve dependencies from other projects' build trees.
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)

# Default to Release (-O3 -DNDEBUG) so a bare `cmake -S . -B build` is fast.
# Multi-config generators (VS) ignore CMAKE_BUILD_TYPE; use --config Release.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release = -O3)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

option(POLYMESH_WITH_OCC "Build B-rep/STEP support via OpenCASCADE (ADR-0001)" OFF)
option(POLYMESH_WITH_CUDA "Build CUDA backends for parallelizable kernels (ADR-0008)" OFF)
option(POLYMESH_WITH_OPENMP "Use OpenMP for parallel assembly / mesh / recovery when available" ON)
option(POLYMESH_WITH_GUI "Build the desktop GUI (GLFW + Dear ImGui + OpenGL)" ON)
option(POLYMESH_WITH_GEOGRAM
    "Vendor Geogram PSM (ConvexCell + predicates) for restricted CVT (G1/ADR-0025)" ON)
option(POLYMESH_BUILD_TESTS "Build the test suite" ON)
# Host CPU ISA tuning. Default OFF: on some GCC+Eigen combos, -march=x86-64-v3
# or -march=native has corrupted heaps (double free) in patch/Tier-0 tests.
# Safe speed comes from -O3 (Release) + OpenMP. Enable only after ctest is green.
option(POLYMESH_NATIVE_ARCH "x86-64-v3 / AVX2 host tuning (OFF = portable + Eigen-safe)" OFF)
# LTO is OFF by default: Eigen + -flto has known ODR/miscompile issues on GCC
# (patch tests SEGV). Enable only for local experiments after ctest.
option(POLYMESH_ENABLE_LTO "Link-time optimization for Release (OFF: Eigen-safe)" OFF)

if(POLYMESH_WITH_CUDA)
    # native = detect local GPU(s). Override e.g. -DCMAKE_CUDA_ARCHITECTURES=86
    # if cross-building or the default arch (often 75) fails to JIT on newer cards.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES native)
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

# Shared warning flags; every target links this.
# COMPILE_LANGUAGE:CXX — do not pass host -W* flags to nvcc (it mis-parses -Werror).
add_library(polymesh_warnings INTERFACE)
target_compile_options(polymesh_warnings INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-Wall -Wextra -Wpedantic -Wconversion -Werror>
)

# Accuracy-preserving performance flags applied to all product libraries/apps.
# Release already gets -O3 -DNDEBUG from CMake; we add host tuning + optional LTO.
# Never -ffast-math / -Ofast (breaks patch tests and Tier-1 double accuracy).
add_library(polymesh_perf INTERFACE)
if(POLYMESH_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    # Prefer x86-64-v3 (AVX2/FMA/BMI) over bare -march=native: on AVX-512 hosts,
    # Eigen's AVX-512 kernels have produced heap corruption (double free) under
    # GCC -O3 in our patch tests. v3 is fast and stable; portable CI can set OFF.
    set(_polymesh_arch_flag "")
    check_cxx_compiler_flag("-march=x86-64-v3" _polymesh_has_v3)
    if(_polymesh_has_v3)
        set(_polymesh_arch_flag "-march=x86-64-v3 -mtune=native")
        message(STATUS "CPU tuning: x86-64-v3 + mtune=native (AVX2/FMA; avoids Eigen AVX-512 bugs)")
    else()
        check_cxx_compiler_flag("-mavx2" _polymesh_has_avx2)
        if(_polymesh_has_avx2)
            set(_polymesh_arch_flag "-mavx2 -mfma -mtune=native")
            message(STATUS "CPU tuning: -mavx2 -mfma -mtune=native")
        else()
            set(_polymesh_arch_flag "-mtune=native")
            message(STATUS "CPU tuning: -mtune=native only")
        endif()
    endif()
    # split flags for INTERFACE options
    separate_arguments(_polymesh_arch_flag_list NATIVE_COMMAND "${_polymesh_arch_flag}")
    target_compile_options(polymesh_perf INTERFACE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:${_polymesh_arch_flag_list}>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>
    )
endif()

if(POLYMESH_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _polymesh_ipo_ok OUTPUT _polymesh_ipo_msg)
    if(_polymesh_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        message(STATUS "LTO: enabled for Release/RelWithDebInfo")
    else()
        message(STATUS "LTO: not supported (${_polymesh_ipo_msg})")
    endif()
endif()

# Prefer multi-threaded Eigen dense kernels when OpenMP is present (still double).
# Do NOT define EIGEN_DONT_PARALLELIZE.
find_package(Eigen3 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

# Resolve OpenMP once for all modules (fea assembly, mesh classify, ZZ, SpMV).
set(POLYMESH_OPENMP_FOUND FALSE)
if(POLYMESH_WITH_OPENMP)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        set(POLYMESH_OPENMP_FOUND TRUE)
        message(STATUS "OpenMP: enabled (assembly, mesh classify, ZZ, SpMV; Eigen stays serial)")
    else()
        message(STATUS "OpenMP: not found — serial hot paths (build still OK)")
    endif()
endif()

# --- third_party (vendored) ---
if(POLYMESH_WITH_GEOGRAM)
    add_subdirectory(third_party/geogram)
    message(STATUS "Geogram: vendored PSM (ConvexCell + predicates) enabled")
else()
    message(STATUS "Geogram: OFF (POLYMESH_WITH_GEOGRAM=OFF) — no clipped-cell kernel")
endif()

# --- libraries (core) ---
add_subdirectory(src/geom)
add_subdirectory(src/mesh)
add_subdirectory(src/adapt)
add_subdirectory(src/fea)
add_subdirectory(src/bench)
add_subdirectory(src/pipeline)

# --- applications (depend on libraries; no reverse deps) ---
add_subdirectory(apps/cli)
add_subdirectory(apps/bench)
add_subdirectory(apps/testlab)
if(POLYMESH_WITH_GUI)
    add_subdirectory(apps/gui)
endif()

if(POLYMESH_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)
    enable_testing()
    add_subdirectory(tests)
endif()
