# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh
    main.cpp
    cli_common.cpp
    commands_mesh.cpp
    commands_solve.cpp
    commands_diag.cpp
    commands_render.cpp
    commands_calibrate.cpp
)
target_link_libraries(polymesh
    PRIVATE polymesh::geom polymesh::mesh polymesh::adapt polymesh::fea polymesh::pipeline
            polymesh::advisor_cost nlohmann_json::nlohmann_json polymesh_warnings polymesh_perf
)
if(TARGET polymesh::advisor)
    target_link_libraries(polymesh PRIVATE polymesh::advisor)
    target_compile_definitions(polymesh PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh)
endif()
