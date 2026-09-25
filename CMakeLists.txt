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

option(POLYMESH_WITH_OCC "Build B-rep/STEP support via OpenCASCADE (ADR-0001)" ON)
option(POLYMESH_WITH_CUDA "Build CUDA backends for parallelizable kernels (ADR-0008)" OFF)
option(POLYMESH_WITH_OPENMP "Use OpenMP for parallel assembly / mesh / recovery when available" ON)
option(POLYMESH_WITH_GUI "Build the desktop GUI (GLFW + Dear ImGui + OpenGL)" ON)
option(POLYMESH_WITH_GEOGRAM
    "Vendor Geogram PSM (ConvexCell + predicates) for restricted CVT (G1/ADR-0025)" ON)
option(POLYMESH_BUILD_TESTS "Build the test suite" ON)
option(POLYMESH_WITH_ADVISOR
    "Build the learned polymesh::mesh polymesh::advisor inference module (ADR-0027; prebuilt ONNX Runtime)" ON)
# Host ISA tuning stays OFF: -march=x86-64-v3/native has corrupted heaps in
# Eigen patch tests under GCC, and v3 made Eigen's Vector3d cross slower (ADR-0044).
option(POLYMESH_NATIVE_ARCH "x86-64-v3 / AVX2 host tuning (OFF = portable + Eigen-safe)" OFF)
# LTO stays OFF: Eigen + GCC -flto has produced ODR/miscompile SEGVs in patch tests.
option(POLYMESH_ENABLE_LTO "Link-time optimization for Release (OFF: Eigen-safe)" OFF)
# Supernodal CHOLMOD makes a quadratic 3-D direct solve viable; without it the
# solver ladder starts at SimplicialLDLT (docs/solver-core.md §6).
option(POLYMESH_WITH_CHOLMOD
    "Use SuiteSparse CHOLMOD's supernodal Cholesky for the direct solve when available" ON)

if(POLYMESH_WITH_CUDA)
    # native = detect local GPU(s); override with -DCMAKE_CUDA_ARCHITECTURES=86 etc.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES native)
    endif()
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
endif()

# Shared warning flags; every target links this. CXX-only: nvcc mis-parses host -W flags.
# POLYMESH_WERROR stays ON for CI and releases; only CI's libc++ determinism
# build turns it off, because libc++ raises an unrelated -Wconversion set.
option(POLYMESH_WERROR "Treat warnings as errors (keep ON for CI and releases)" ON)
add_library(polymesh_warnings INTERFACE)
target_compile_options(polymesh_warnings INTERFACE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-Wall -Wextra -Wpedantic -Wconversion>
    $<$<AND:$<BOOL:${POLYMESH_WERROR}>,$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-Werror>
)

# Accuracy-preserving performance flags for product targets: optional host
# tuning only. Never -ffast-math / -Ofast (patch tests and Tier-1 need exact double).
add_library(polymesh_perf INTERFACE)
if(POLYMESH_NATIVE_ARCH)
    include(CheckCXXCompilerFlag)
    # Prefer x86-64-v3 over -march=native: Eigen's AVX-512 kernels corrupted heaps under GCC -O3.
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
    # Split flags for INTERFACE options.
    separate_arguments(_polymesh_arch_flag_list NATIVE_COMMAND "${_polymesh_arch_flag}")
    target_compile_options(polymesh_perf INTERFACE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:${_polymesh_arch_flag_list}>
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/arch:AVX2>
    )
endif()

if(WIN32)
    # windows.h min/max macros break std::max (MSVC C2589) in any TU that includes it.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
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

# An embedding host (add_subdirectory) may already provide these targets; do not
# re-resolve them, or Eigen/JSON versions can mix across the static-library boundary.
if(NOT TARGET Eigen3::Eigen)
    find_package(Eigen3 REQUIRED)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11 REQUIRED)
endif()

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

# Resolve SuiteSparse CHOLMOD once: pkg-config (CHOLMOD.pc) first, then the
# SuiteSparse >= 7 CMake config. Neither found is not an error. pkg-config goes
# first because a broken distro config chain (Fedora 44: CCOLAMD's missing
# *Targets_static.cmake) aborts configure even under QUIET, superbuilds included.
set(POLYMESH_CHOLMOD_FOUND FALSE)
if(POLYMESH_WITH_CHOLMOD)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(POLYMESH_PC_CHOLMOD QUIET CHOLMOD)
    endif()
    if(POLYMESH_PC_CHOLMOD_FOUND)
        set(POLYMESH_CHOLMOD_FOUND TRUE)
        add_library(polymesh_cholmod INTERFACE)
        target_include_directories(polymesh_cholmod SYSTEM INTERFACE
                                   ${POLYMESH_PC_CHOLMOD_INCLUDE_DIRS})
        target_link_libraries(polymesh_cholmod INTERFACE ${POLYMESH_PC_CHOLMOD_LINK_LIBRARIES})
        message(STATUS "CHOLMOD: enabled via pkg-config ${POLYMESH_PC_CHOLMOD_VERSION} "
                       "(supernodal direct solve)")
    else()
        find_package(CHOLMOD QUIET CONFIG)
        if(TARGET CHOLMOD::CHOLMOD)
            set(POLYMESH_CHOLMOD_FOUND TRUE)
            add_library(polymesh_cholmod INTERFACE)
            target_link_libraries(polymesh_cholmod INTERFACE CHOLMOD::CHOLMOD)
            message(STATUS
                "CHOLMOD: enabled via SuiteSparse CMake config (supernodal direct solve)")
        else()
            message(STATUS "CHOLMOD: not found — direct solve falls back to "
                           "Eigen SimplicialLDLT (install suitesparse / libsuitesparse-dev)")
        endif()
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
add_subdirectory(src/advisor)

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
