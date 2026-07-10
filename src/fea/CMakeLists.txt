# SPDX-License-Identifier: BSD-3-Clause
add_library(fea
    src/material.cpp
    src/backend.cpp
    src/nodal_mesh.cpp
    src/quadrature.cpp
    src/shape.cpp
    src/assembly.cpp
    src/solve.cpp
    src/traction.cpp
    src/stress.cpp
    src/msh.cpp
    src/vtu.cpp
    src/zz.cpp
    src/vem.cpp
)
target_include_directories(fea PUBLIC include)
target_link_libraries(fea PUBLIC mesh Eigen3::Eigen PRIVATE polymesh_warnings)

if(POLYMESH_WITH_CUDA)
    target_sources(fea PRIVATE src/backend_cuda.cu)
    target_compile_definitions(fea PUBLIC POLYMESH_WITH_CUDA)
    target_link_libraries(fea PUBLIC CUDA::cudart)
endif()

# Optional OpenMP for assemble_stiffness (F1). Build remains serial if missing.
if(POLYMESH_WITH_OPENMP)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(fea PUBLIC OpenMP::OpenMP_CXX)
        target_compile_definitions(fea PUBLIC POLYMESH_WITH_OPENMP)
        message(STATUS "OpenMP: enabled for fea element stiffness assembly")
    else()
        message(STATUS "OpenMP: not found — serial stiffness assembly")
    endif()
endif()
