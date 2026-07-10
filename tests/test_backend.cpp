// SPDX-License-Identifier: BSD-3-Clause
#include "fea/backend.hpp"

#include <catch2/catch_test_macros.hpp>

using polymesh::fea::active_backend;
using polymesh::fea::Backend;
using polymesh::fea::backend_description;

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

#ifndef POLYMESH_WITH_CUDA
TEST_CASE("without CUDA compiled in, backend is cpu") {
    CHECK(active_backend() == Backend::kCpu);
}
#endif
