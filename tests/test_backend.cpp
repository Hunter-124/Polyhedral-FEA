// SPDX-License-Identifier: BSD-3-Clause
#include "fea/backend.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using polymesh::fea::active_backend;
using polymesh::fea::Backend;
using polymesh::fea::backend_description;
using polymesh::fea::init_runtime_performance;
using polymesh::fea::openmp_enabled;
using polymesh::fea::openmp_max_threads;
using polymesh::fea::performance_description;

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

#ifndef POLYMESH_WITH_CUDA
TEST_CASE("without CUDA compiled in, backend is cpu") {
    CHECK(active_backend() == Backend::kCpu);
}
#endif
