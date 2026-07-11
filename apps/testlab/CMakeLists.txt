# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh_testlab main.cpp)
target_link_libraries(polymesh_testlab
    PRIVATE geom mesh adapt fea pipeline bench_harness
            nlohmann_json::nlohmann_json
            polymesh_warnings polymesh_perf
)
# Binary is launched from repo root so campaign/case/reference paths resolve.
