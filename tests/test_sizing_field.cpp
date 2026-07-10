// SPDX-License-Identifier: BSD-3-Clause
#include "adapt/sizing_field.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("uniform sizing is constant") {
    const polymesh::adapt::UniformSizing f(0.05);
    CHECK(f.size_at({0.0, 0.0, 0.0}) == 0.05);
    CHECK(f.size_at({1e3, -2e3, 5.5}) == 0.05);
}
