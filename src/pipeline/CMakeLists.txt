# SPDX-License-Identifier: BSD-3-Clause
# Headless app pipeline: import → regions → mesh → solve.
# No GUI/OpenGL dependencies. apps/gui and tests both consume this.

add_library(pipeline
    src/scene.cpp
)
target_include_directories(pipeline PUBLIC include)
target_link_libraries(pipeline
    PUBLIC geom mesh fea adapt Eigen3::Eigen
    PRIVATE polymesh_warnings
)
