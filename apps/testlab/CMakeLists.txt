# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh_testlab main.cpp)
target_include_directories(polymesh_testlab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(polymesh_testlab
    PRIVATE geom mesh adapt fea pipeline bench_harness
            nlohmann_json::nlohmann_json
            polymesh_warnings polymesh_perf
)
if(TARGET advisor)
    target_link_libraries(polymesh_testlab PRIVATE advisor)
    target_compile_definitions(polymesh_testlab PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh_testlab)
endif()
# Binary is launched from repo root so campaign/case/reference paths resolve.
