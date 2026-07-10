# SPDX-License-Identifier: BSD-3-Clause
add_library(bench_harness
    src/reference_case.cpp
)
target_include_directories(bench_harness PUBLIC include)
target_link_libraries(bench_harness
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE polymesh_warnings
)
