# SPDX-License-Identifier: BSD-3-Clause
add_library(polymesh_bench_harness
    src/reference_case.cpp
)
add_library(polymesh::bench_harness ALIAS polymesh_bench_harness)
target_include_directories(polymesh_bench_harness PUBLIC include)
target_link_libraries(polymesh_bench_harness
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE polymesh_warnings
)
