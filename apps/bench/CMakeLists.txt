# SPDX-License-Identifier: BSD-3-Clause
# Optional offline benches (not multi-minute ctest gates).

add_executable(polymesh-d6-tier3 d6_tier3.cpp)
target_link_libraries(polymesh-d6-tier3
    PRIVATE fea bench_harness polymesh_warnings
)
# Working directory for load_reference is the repo root when launched by
# bench/d6/run_tier3.py; binary itself does not chdir.
