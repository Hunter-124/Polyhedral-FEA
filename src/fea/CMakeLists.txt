# SPDX-License-Identifier: BSD-3-Clause
add_library(polymesh_fea
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
    src/constraints.cpp
    src/solve.cpp
    src/resource_budget.cpp
    src/traction.cpp
    src/stress.cpp
    src/cell_quality.cpp
    src/element_validity.cpp
    src/msh.cpp
    src/vtu.cpp
    src/zz.cpp
    src/vem.cpp
    src/p_elevate.cpp
    src/poly_to_vem.cpp
)
add_library(polymesh::fea ALIAS polymesh_fea)
target_include_directories(polymesh_fea PUBLIC include)
target_link_libraries(polymesh_fea PUBLIC polymesh::mesh Eigen3::Eigen PRIVATE polymesh_warnings polymesh_perf)

if(POLYMESH_WITH_CUDA)
    target_sources(polymesh_fea PRIVATE src/backend_cuda.cu)
    target_compile_definitions(polymesh_fea PUBLIC POLYMESH_WITH_CUDA)
    target_link_libraries(polymesh_fea PUBLIC CUDA::cudart)
endif()

# OpenMP for assemble_stiffness, ZZ recovery, stress, SpMV (F1+). Serial if missing.
if(POLYMESH_OPENMP_FOUND)
    target_link_libraries(polymesh_fea PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(polymesh_fea PUBLIC POLYMESH_WITH_OPENMP)
endif()
