// SPDX-License-Identifier: BSD-3-Clause

// Unit tests for apps/testlab/probe_util.hpp — face-mean vs global-max contract.

#include "probe_util.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <vector>

using polymesh::testlab::face_mean_displacement_component;
using polymesh::testlab::face_mean_displacement_mag;
using polymesh::testlab::global_max_displacement_mag;

TEST_CASE("probe_util: face-mean |u| != global max on synthetic 2-node field") {
    // Node 0: |u| = 1.0; node 1: |u| = 100.0. Face-mean of {0,1} = 50.5;
    // mean of only tip node {0} = 1.0; global max = 100.
    Eigen::VectorXd u(6);
    u << 1.0, 0.0, 0.0, 0.0, 0.0, 100.0;

    const std::vector<std::uint32_t> tip_only = {0};
    const std::vector<std::uint32_t> both = {0, 1};

    const double mean_tip = face_mean_displacement_mag(u, tip_only);
    const double mean_both = face_mean_displacement_mag(u, both);
    const double gmax = global_max_displacement_mag(u, 2);

    CHECK_THAT(mean_tip, Catch::Matchers::WithinAbs(1.0, 1e-12));
    CHECK_THAT(mean_both, Catch::Matchers::WithinAbs(50.5, 1e-12));
    CHECK_THAT(gmax, Catch::Matchers::WithinAbs(100.0, 1e-12));
    CHECK(mean_tip != gmax);
    CHECK(mean_both != gmax);
    CHECK(mean_tip < gmax);
}

TEST_CASE("probe_util: signed component mean uses load axis") {
    Eigen::VectorXd u(6);
    // node0: ux=-2; node1: ux=-4  → mean_ux = -3
    u << -2.0, 0.5, 0.0, -4.0, 1.0, 0.0;
    const std::vector<std::uint32_t> nodes = {0, 1};
    CHECK_THAT(face_mean_displacement_component(u, nodes, 0),
               Catch::Matchers::WithinAbs(-3.0, 1e-12));
    CHECK_THAT(face_mean_displacement_component(u, nodes, 1),
               Catch::Matchers::WithinAbs(0.75, 1e-12));
}

TEST_CASE("probe_util: empty node set returns zero") {
    Eigen::VectorXd u(3);
    u << 9.0, 0.0, 0.0;
    CHECK(face_mean_displacement_mag(u, {}) == 0.0);
}
