# SPDX-License-Identifier: BSD-3-Clause
# Headless app pipeline: import → regions → mesh → solve.
# No GUI/OpenGL dependencies. apps/gui and tests both consume this.

add_library(polymesh_pipeline
    src/scene.cpp
    src/surface_render.cpp
)
add_library(polymesh::pipeline ALIAS polymesh_pipeline)
target_include_directories(polymesh_pipeline PUBLIC include)
target_link_libraries(polymesh_pipeline
    PUBLIC polymesh::geom polymesh::mesh polymesh::fea polymesh::adapt Eigen3::Eigen
    PRIVATE polymesh_warnings polymesh_perf
)
