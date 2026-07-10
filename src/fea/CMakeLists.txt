# SPDX-License-Identifier: AGPL-3.0-or-later
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
)
target_include_directories(fea PUBLIC include)
target_link_libraries(fea PUBLIC mesh Eigen3::Eigen PRIVATE polymesh_warnings)

if(POLYMESH_WITH_CUDA)
    target_sources(fea PRIVATE src/backend_cuda.cu)
    target_compile_definitions(fea PUBLIC POLYMESH_WITH_CUDA)
    target_link_libraries(fea PUBLIC CUDA::cudart)
endif()
