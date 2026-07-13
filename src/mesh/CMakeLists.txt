# SPDX-License-Identifier: BSD-3-Clause
add_library(mesh
    src/poly_mesh.cpp
    src/tet_fill.cpp
    src/hex_fill.cpp
    src/prism_fill.cpp
    src/quality.cpp
    src/surface_project.cpp
    src/surface_metrics.cpp
    src/grid_classify.cpp
    src/cell_stamp.cpp
    src/hybrid_fill.cpp
    src/mixed_fill.cpp
    src/octa_fill.cpp
    src/transition_fill.cpp
    src/local_refine.cpp
    src/varyhedron_fill.cpp
    src/wall_project.cpp
    src/geogram_clip.cpp
    src/cvt_lloyd.cpp
    src/cvt_sites.cpp
    src/cvt_export.cpp
)
target_include_directories(mesh PUBLIC include)
target_link_libraries(mesh PUBLIC geom Eigen3::Eigen PRIVATE polymesh_warnings polymesh_perf)

# G1: vendored Geogram ConvexCell / predicates (ADR-0025). Default ON.
if(POLYMESH_WITH_GEOGRAM)
    target_link_libraries(mesh PUBLIC polymesh_geogram)
    target_compile_definitions(mesh PUBLIC POLYMESH_WITH_GEOGRAM=1)
else()
    target_compile_definitions(mesh PUBLIC POLYMESH_WITH_GEOGRAM=0)
endif()

# OpenMP for Cartesian inside-tests (grid classify) and similar mesh hot loops.
if(POLYMESH_OPENMP_FOUND)
    target_link_libraries(mesh PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(mesh PUBLIC POLYMESH_WITH_OPENMP)
endif()
