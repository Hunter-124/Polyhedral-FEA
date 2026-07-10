# SPDX-License-Identifier: BSD-3-Clause
cmake_minimum_required(VERSION 3.24)
project(polymesh VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Never resolve dependencies from other projects' build trees.
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)

option(POLYMESH_WITH_OCC "Build B-rep/STEP support via OpenCASCADE (ADR-0001)" OFF)
option(POLYMESH_WITH_CUDA "Build CUDA backends for parallelizable kernels (ADR-0008)" OFF)
option(POLYMESH_WITH_OPENMP "Use OpenMP for element stiffness assembly when available" ON)
option(POLYMESH_WITH_GUI "Build the desktop GUI (GLFW + Dear ImGui + OpenGL)" ON)
option(POLYMESH_BUILD_TESTS "Build the test suite" ON)

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

find_package(Eigen3 REQUIRED)
find_package(nlohmann_json 3.11 REQUIRED)

# --- libraries (core) ---
add_subdirectory(src/geom)
add_subdirectory(src/mesh)
add_subdirectory(src/adapt)
add_subdirectory(src/fea)
add_subdirectory(src/bench)
add_subdirectory(src/pipeline)

# --- applications (depend on libraries; no reverse deps) ---
add_subdirectory(apps/cli)
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
