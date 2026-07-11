# SPDX-License-Identifier: BSD-3-Clause
add_library(fea
    src/material.cpp
    src/backend.cpp
    src/spmv.cpp
    src/nodal_mesh.cpp
    src/boundary_faces.cpp
    src/quadrature.cpp
    src/shape.cpp
    src/hierarchical.cpp
    src/hp_assembly.cpp
    src/assembly.cpp
    src/solve.cpp
    src/traction.cpp
    src/stress.cpp
    src/msh.cpp
    src/vtu.cpp
    src/zz.cpp
    src/vem.cpp
    src/p_elevate.cpp
)
target_include_directories(fea PUBLIC include)
target_link_libraries(fea PUBLIC mesh Eigen3::Eigen PRIVATE polymesh_warnings polymesh_perf)

if(POLYMESH_WITH_CUDA)
    target_sources(fea PRIVATE src/backend_cuda.cu)
    target_compile_definitions(fea PUBLIC POLYMESH_WITH_CUDA)
    target_link_libraries(fea PUBLIC CUDA::cudart)
endif()

# OpenMP for assemble_stiffness, ZZ recovery, stress, SpMV (F1+). Serial if missing.
if(POLYMESH_OPENMP_FOUND)
    target_link_libraries(fea PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(fea PUBLIC POLYMESH_WITH_OPENMP)
endif()
