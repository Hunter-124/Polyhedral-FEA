// SPDX-License-Identifier: BSD-3-Clause
#include "fea/backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using polymesh::fea::active_backend;
using polymesh::fea::Backend;
using polymesh::fea::backend_description;
using polymesh::fea::init_runtime_performance;
using polymesh::fea::openmp_default_threads;
using polymesh::fea::openmp_enabled;
using polymesh::fea::openmp_max_threads;
using polymesh::fea::performance_description;
using polymesh::fea::set_openmp_threads;

TEST_CASE("backend is consistent with its description") {
    switch (active_backend()) {
    case Backend::kCpu:
        CHECK(backend_description() == "cpu");
        break;
    case Backend::kCuda:
        CHECK(backend_description().starts_with("cuda ("));
        break;
    }
}

TEST_CASE("performance runtime reports OpenMP thread count") {
    init_runtime_performance();
    CHECK(openmp_max_threads() >= 1);
    const auto desc = performance_description();
    CHECK_FALSE(desc.empty());
    if (openmp_enabled()) {
        CHECK(desc.find("OpenMP") != std::string::npos);
    } else {
        CHECK(desc.find("serial") != std::string::npos);
    }
}

TEST_CASE("set_openmp_threads caps and restores default") {
    init_runtime_performance();
    const int def = openmp_default_threads();
    REQUIRE(def >= 1);

    set_openmp_threads(1);
    CHECK(openmp_max_threads() == 1);

    // 0 restores the process default captured at init.
    set_openmp_threads(0);
    CHECK(openmp_max_threads() == def);

    // Over-large request clamps to default (never above hardware/process max).
    set_openmp_threads(def + 1000);
    CHECK(openmp_max_threads() == def);

    set_openmp_threads(0); // leave suite at default
}

#ifndef POLYMESH_WITH_CUDA
TEST_CASE("without CUDA compiled in, backend is cpu") {
    CHECK(active_backend() == Backend::kCpu);
}
#endif
