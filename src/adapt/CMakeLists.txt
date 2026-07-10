# SPDX-License-Identifier: AGPL-3.0-or-later
# Header-only for now; gains sources in Phase P5.
add_library(adapt INTERFACE)
target_include_directories(adapt INTERFACE include)
target_link_libraries(adapt INTERFACE mesh Eigen3::Eigen)
