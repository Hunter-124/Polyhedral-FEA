# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh_testlab main.cpp)
target_include_directories(polymesh_testlab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(polymesh_testlab
    PRIVATE polymesh::geom polymesh::mesh polymesh::adapt polymesh::fea polymesh::pipeline
            polymesh::bench_harness polymesh::advisor_cost nlohmann_json::nlohmann_json
            polymesh_warnings polymesh_perf
)
if(TARGET polymesh::advisor)
    target_link_libraries(polymesh_testlab PRIVATE polymesh::advisor)
    target_compile_definitions(polymesh_testlab PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh_testlab)
endif()
# Binary is launched from repo root so campaign/case/reference paths resolve.
