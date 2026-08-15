# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh main.cpp)
target_link_libraries(polymesh
    PRIVATE polymesh::geom polymesh::mesh polymesh::adapt polymesh::fea polymesh::pipeline polymesh_warnings polymesh_perf
)
if(TARGET polymesh::advisor)
    target_link_libraries(polymesh PRIVATE polymesh::advisor)
    target_compile_definitions(polymesh PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh)
endif()
