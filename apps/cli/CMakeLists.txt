# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh main.cpp)
target_link_libraries(polymesh
    PRIVATE geom mesh adapt fea pipeline polymesh_warnings polymesh_perf
)
if(TARGET advisor)
    target_link_libraries(polymesh PRIVATE advisor)
    target_compile_definitions(polymesh PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh)
endif()
