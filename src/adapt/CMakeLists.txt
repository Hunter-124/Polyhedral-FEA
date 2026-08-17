# SPDX-License-Identifier: BSD-3-Clause
add_library(polymesh_adapt
    src/error.cpp
    src/graded_sizing.cpp
    src/metric_field.cpp
    src/loop.cpp
    src/sizing_field.cpp
    src/spectral_sizing.cpp
    src/hp_driver.cpp
)
add_library(polymesh::adapt ALIAS polymesh_adapt)
target_include_directories(polymesh_adapt PUBLIC include)
target_link_libraries(polymesh_adapt PUBLIC polymesh::geom polymesh::mesh Eigen3::Eigen PRIVATE polymesh_warnings polymesh_perf)
