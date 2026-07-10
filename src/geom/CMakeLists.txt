# SPDX-License-Identifier: AGPL-3.0-or-later
add_library(geom
    src/tri_surface.cpp
    src/stl.cpp
)
target_include_directories(geom PUBLIC include)
target_link_libraries(geom PUBLIC Eigen3::Eigen PRIVATE polymesh_warnings)

if(POLYMESH_WITH_OCC)
    # Wired up when Phase P3 starts consuming exact geometry (ADR-0001).
    find_package(OpenCASCADE REQUIRED)
    target_compile_definitions(geom PUBLIC POLYMESH_WITH_OCC)
endif()
