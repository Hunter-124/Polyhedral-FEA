# SPDX-License-Identifier: BSD-3-Clause
add_library(mesh
    src/poly_mesh.cpp
)
target_include_directories(mesh PUBLIC include)
target_link_libraries(mesh PUBLIC geom Eigen3::Eigen PRIVATE polymesh_warnings)
