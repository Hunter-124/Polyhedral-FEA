# SPDX-License-Identifier: BSD-3-Clause
# Headless app pipeline: import → regions → mesh → solve.
# No GUI/OpenGL dependencies. apps/gui and tests both consume this.

add_library(polymesh_pipeline
    src/cad_pressure.cpp
    src/case_features.cpp
    src/curved_geometry.cpp
    src/mesh_size.cpp
    src/mesher_policy.cpp
    src/model.cpp
    src/png_encode.cpp
    src/refinement_plan.cpp
    src/ship_gate.cpp
    src/solve_job.cpp
    src/surface_render.cpp
    src/volume_mesh.cpp
)
add_library(polymesh::pipeline ALIAS polymesh_pipeline)
target_include_directories(polymesh_pipeline PUBLIC include)
target_link_libraries(polymesh_pipeline
    PUBLIC polymesh::geom polymesh::mesh polymesh::fea polymesh::adapt Eigen3::Eigen
    PRIVATE polymesh_warnings polymesh_perf
)
