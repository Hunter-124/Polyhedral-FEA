// SPDX-License-Identifier: BSD-3-Clause
// Unit tests for the hierarchical (modal) arbitrary-p basis (ADR-0019).
#include "fea/assembly.hpp"
#include "fea/hierarchical.hpp"
#include "fea/nodal_mesh.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <Eigen/Dense>

#include <vector>

using namespace polymesh::fea;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// P_n(x) by recurrence, for an independent check of the Lobatto derivative.
double legendre(int n, double x) {
    if (n == 0) {
        return 1.0;
    }
    double a = 1.0, b = x;
    for (int k = 1; k < n; ++k) {
        const double c = (static_cast<double>(2 * k + 1) * x * b - static_cast<double>(k) * a) /
                         static_cast<double>(k + 1);
        a = b;
        b = c;
    }
    return b;
}

// Count near-zero eigenvalues of a symmetric matrix relative to its largest.
int count_zero_modes(const Eigen::MatrixXd& k, double rel_tol) {
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(k);
    const Eigen::VectorXd ev = es.eigenvalues();
    const double scale = ev.cwiseAbs().maxCoeff();
    int zeros = 0;
    for (Eigen::Index i = 0; i < ev.size(); ++i) {
        if (std::abs(ev(i)) <= rel_tol * scale) {
            ++zeros;
        }
    }
    return zeros;
}

Eigen::Matrix<double, Eigen::Dynamic, 3> unit_hex_coords() {
    Eigen::Matrix<double, 8, 3> x;
    x << 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1;
    return x;
}

Eigen::Matrix<double, Eigen::Dynamic, 3> unit_tet_coords() {
    Eigen::Matrix<double, 4, 3> x;
    x << 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1;
    return x;
}

} // namespace

TEST_CASE("Lobatto bubbles vanish at the endpoints", "[hierarchical]") {
    for (int k = 2; k <= 6; ++k) {
        CHECK_THAT(lobatto::value(k, 1.0), WithinAbs(0.0, 1e-13));
        CHECK_THAT(lobatto::value(k, -1.0), WithinAbs(0.0, 1e-13));
    }
    // Vertex modes partition unity.
    for (double xi : {-0.7, 0.0, 0.3, 1.0}) {
        CHECK_THAT(lobatto::value(0, xi) + lobatto::value(1, xi), WithinAbs(1.0, 1e-13));
    }
}

TEST_CASE("Lobatto derivative matches the integrated-Legendre identity", "[hierarchical]") {
    // phi_k'(xi) = sqrt((2k-1)/2) * P_{k-1}(xi).
    for (int k = 2; k <= 6; ++k) {
        for (double xi : {-0.9, -0.3, 0.15, 0.6}) {
            const double expected = std::sqrt((2.0 * k - 1.0) / 2.0) * legendre(k - 1, xi);
            CHECK_THAT(lobatto::deriv(k, xi), WithinRel(expected, 1e-12));
        }
    }
}

TEST_CASE("Lobatto parity: phi_k(-xi) = (-1)^k phi_k(xi)", "[hierarchical]") {
    for (int k = 2; k <= 6; ++k) {
        for (double xi : {0.1, 0.4, 0.85}) {
            const double sign = (k % 2 == 0) ? 1.0 : -1.0;
            CHECK_THAT(lobatto::value(k, -xi), WithinAbs(sign * lobatto::value(k, xi), 1e-12));
        }
    }
}

TEST_CASE("Hierarchical mode counts follow the entity tally", "[hierarchical]") {
    // Hex uniform p: (p+1)^3.
    CHECK(hp_num_modes(ElementType::kHex8, 1) == 8);
    CHECK(hp_num_modes(ElementType::kHex8, 2) == 27);
    CHECK(hp_num_modes(ElementType::kHex8, 3) == 64);
    CHECK(hp_num_modes(ElementType::kHex8, 4) == 125);
    CHECK(hp_num_modes(ElementType::kHex8, 5) == 216);
    CHECK(hp_num_modes(ElementType::kHex8, 6) == 343);
    // Tet complete P_p: (p+1)(p+2)(p+3)/6.
    CHECK(hp_num_modes(ElementType::kTet4, 1) == 4);
    CHECK(hp_num_modes(ElementType::kTet4, 2) == 10);
    CHECK(hp_num_modes(ElementType::kTet4, 3) == 20);
    CHECK(hp_num_modes(ElementType::kTet4, 4) == 35);
    // First entries are always the vertices (so p=1 is the nodal restriction).
    const auto modes = hp_modes(ElementType::kHex8, 3);
    for (int v = 0; v < 8; ++v) {
        CHECK(modes[static_cast<std::size_t>(v)].entity == HpMode::Entity::kVertex);
    }
}

TEST_CASE("p=1 hierarchical stiffness equals the nodal element", "[hierarchical]") {
    const Material steel{2.1e11, 0.3};

    SECTION("hex") {
        const auto x = unit_hex_coords();
        NodalMesh mesh;
        for (Eigen::Index i = 0; i < 8; ++i) {
            mesh.nodes.push_back(x.row(i).transpose());
        }
        mesh.elements.push_back(NodalElement{ElementType::kHex8, {0, 1, 2, 3, 4, 5, 6, 7}});
        const Eigen::MatrixXd k_nodal = element_stiffness(mesh, mesh.elements[0], steel);
        const Eigen::MatrixXd k_hp = hp_element_stiffness(x, ElementType::kHex8, 1, steel);
        REQUIRE(k_hp.rows() == k_nodal.rows());
        CHECK((k_hp - k_nodal).norm() <= 1e-6 * k_nodal.norm());
    }

    SECTION("tet") {
        const auto x = unit_tet_coords();
        NodalMesh mesh;
        for (Eigen::Index i = 0; i < 4; ++i) {
            mesh.nodes.push_back(x.row(i).transpose());
        }
        mesh.elements.push_back(NodalElement{ElementType::kTet4, {0, 1, 2, 3}});
        const Eigen::MatrixXd k_nodal = element_stiffness(mesh, mesh.elements[0], steel);
        const Eigen::MatrixXd k_hp = hp_element_stiffness(x, ElementType::kTet4, 1, steel);
        REQUIRE(k_hp.rows() == k_nodal.rows());
        CHECK((k_hp - k_nodal).norm() <= 1e-6 * k_nodal.norm());
    }
}

TEST_CASE("Hierarchical stiffness has exactly six rigid-body modes", "[hierarchical]") {
    const Material steel{2.1e11, 0.3};

    SECTION("hex, orders 1..4, distorted geometry") {
        auto x = unit_hex_coords();
        x.row(6) += Eigen::RowVector3d(0.15, -0.1, 0.2); // break axis alignment
        for (std::uint8_t p = 1; p <= 4; ++p) {
            const Eigen::MatrixXd k = hp_element_stiffness(x, ElementType::kHex8, p, steel);
            INFO("hex order " << int(p) << " ndof " << k.rows());
            CHECK(count_zero_modes(k, 1e-9) == 6);
        }
    }

    SECTION("tet, orders 1..4, distorted geometry") {
        auto x = unit_tet_coords();
        x.row(3) += Eigen::RowVector3d(0.1, 0.05, 0.0);
        for (std::uint8_t p = 1; p <= 4; ++p) {
            const Eigen::MatrixXd k = hp_element_stiffness(x, ElementType::kTet4, p, steel);
            INFO("tet order " << int(p) << " ndof " << k.rows());
            CHECK(count_zero_modes(k, 1e-9) == 6);
        }
    }
}

TEST_CASE("Hierarchical stiffness is symmetric positive semidefinite", "[hierarchical]") {
    const Material steel{2.1e11, 0.3};
    const auto x = unit_hex_coords();
    for (std::uint8_t p = 1; p <= 4; ++p) {
        const Eigen::MatrixXd k = hp_element_stiffness(x, ElementType::kHex8, p, steel);
        CHECK((k - k.transpose()).norm() <= 1e-6 * k.norm());
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(k);
        CHECK(es.eigenvalues().minCoeff() >= -1e-9 * es.eigenvalues().cwiseAbs().maxCoeff());
    }
}

TEST_CASE("Tet edge/face bubbles vanish on the complementary skeleton", "[hierarchical]") {
    // At a point on face opposite vertex 3 (zeta=0 plane of ref tet has λ3=0
    // wait — ref tet: λ0=1-x-y-z, λ1=x, λ2=y, λ3=z. Face opposite v3 is z=0.
    // Edge (0,1) bubble must vanish on faces that do not contain that edge.
    const auto field2 = hp_eval(ElementType::kTet4, 2, {0.2, 0.3, 0.0}); // on face 012
    const auto modes2 = hp_modes(ElementType::kTet4, 2);
    // Edge (0,3) = local edge 3 does not lie on face 012 (z=0); its bubble ~ λ0 λ3 = 0.
    for (std::size_t m = 0; m < modes2.size(); ++m) {
        if (modes2[m].entity == HpMode::Entity::kEdge && modes2[m].entity_index == 3) {
            CHECK_THAT(field2.n(static_cast<Eigen::Index>(m)), WithinAbs(0.0, 1e-13));
        }
    }
    // p=3 face bubble on face 012 must vanish on the edges of that face? It is
    // λ0 λ1 λ2, which vanishes on each edge of face 012 (one λ=0). Interior
    // sample on face is nonzero.
    const auto field3 = hp_eval(ElementType::kTet4, 3, {0.25, 0.25, 0.0});
    const auto modes3 = hp_modes(ElementType::kTet4, 3);
    bool saw_face = false;
    for (std::size_t m = 0; m < modes3.size(); ++m) {
        if (modes3[m].entity == HpMode::Entity::kFace && modes3[m].entity_index == 0) {
            saw_face = true;
            CHECK(std::abs(field3.n(static_cast<Eigen::Index>(m))) > 1e-6);
        }
    }
    CHECK(saw_face);
}
