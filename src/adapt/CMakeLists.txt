# SPDX-License-Identifier: BSD-3-Clause
add_library(adapt
    src/error.cpp
)
target_include_directories(adapt PUBLIC include)
target_link_libraries(adapt PUBLIC geom mesh Eigen3::Eigen PRIVATE polymesh_warnings)
