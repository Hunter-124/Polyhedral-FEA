# SPDX-License-Identifier: BSD-3-Clause
add_library(mesh
    src/poly_mesh.cpp
    src/tet_fill.cpp
    src/hex_fill.cpp
    src/quality.cpp
    src/surface_project.cpp
    src/hybrid_fill.cpp
    src/transition_fill.cpp
    src/local_refine.cpp
)
target_include_directories(mesh PUBLIC include)
target_link_libraries(mesh PUBLIC geom Eigen3::Eigen PRIVATE polymesh_warnings)
