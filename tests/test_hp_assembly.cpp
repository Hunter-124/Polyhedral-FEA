// SPDX-License-Identifier: BSD-3-Clause
// Conforming hierarchical assembly (ADR-0019): the minimum rule keeps mixed
// order conforming (constant-strain patch across a p1/p2 interface), and an
// MMS problem converges in the energy norm at the theoretical rate p — the
// end-to-end proof that shared entity DOFs (with p>=3 orientation signs and
// hex face transforms) are assembled correctly.
#include "fea/hp_assembly.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <numbers>
#include <vector>

using namespace polymesh::fea;
using polymesh::test_support::box_hex_mesh;

namespace {

const Material kSteel{2.1e11, 0.3};

HpModel to_hp(const NodalMesh& mesh, std::uint8_t order) {
    HpModel m;
    m.nodes = mesh.nodes;
    for (const auto& e : mesh.elements) {
        m.elements.push_back(HpElementDef{e.type, e.nodes, order});
    }
    return m;
}

// True if node id lies on the boundary plane the entity shares (used to pin
// homogeneous Dirichlet on all boundary modes).
bool modes_on_boundary(const std::vector<std::uint32_t>& nodes,
                       const std::vector<Eigen::Vector3d>& coords, double lo, double hi) {
    for (int axis = 0; axis < 3; ++axis) {
        for (double plane : {lo, hi}) {
            bool all = true;
            for (auto n : nodes) {
                if (std::abs(coords[n][axis] - plane) > 1e-9) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return true;
            }
        }
    }
    return false;
}

std::map<Eigen::Index, double> homogeneous_boundary(const HpModel& model, const HpSystem& sys) {
    std::map<Eigen::Index, double> fixed;
    for (Eigen::Index m = 0; m < sys.n_modes; ++m) {
        const auto& nodes = sys.mode_nodes[static_cast<std::size_t>(m)];
        const bool bdry =
            (nodes.size() == 1)
                ? [&] {
                      const Eigen::Vector3d& p = model.nodes[nodes[0]];
                      return p.x() < 1e-9 || p.x() > 1 - 1e-9 || p.y() < 1e-9 ||
                             p.y() > 1 - 1e-9 || p.z() < 1e-9 || p.z() > 1 - 1e-9;
                  }()
                : modes_on_boundary(nodes, model.nodes, 0.0, 1.0);
        if (bdry) {
            for (int a = 0; a < 3; ++a) {
                fixed[3 * m + a] = 0.0;
            }
        }
    }
    return fixed;
}

} // namespace

TEST_CASE("minimum rule keeps mixed p1/p2 order conforming (constant strain)",
          "[hierarchical][hp]") {
    // Two hexes sharing a face; one order 2, one order 1. A linear field must
    // be reproduced exactly across the mixed-order interface. Unit cube split
    // in x, so the shared face at x=0.5 is interior and boundary planes are
    // the standard {0,1} on every axis.
    const auto mesh = box_hex_mesh(2, 1, 1, Eigen::Vector3d(1.0, 1.0, 1.0));
    HpModel model = to_hp(mesh, 1);
    REQUIRE(model.elements.size() == 2);
    model.elements[0].order = 2; // mixed order across the shared face

    const auto sys = assemble_hp(model, kSteel);

    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, 3e-4, -8e-4, 5e-4, -6e-4, 2e-4, 7e-4;

    std::map<Eigen::Index, double> fixed;
    for (Eigen::Index m = 0; m < sys.n_modes; ++m) {
        const auto& nodes = sys.mode_nodes[static_cast<std::size_t>(m)];
        if (nodes.size() == 1) {
            const Eigen::Vector3d& p = model.nodes[nodes[0]];
            const bool on_bdry = p.x() < 1e-9 || p.x() > 1 - 1e-9 || p.y() < 1e-9 ||
                                 p.y() > 1 - 1e-9 || p.z() < 1e-9 || p.z() > 1 - 1e-9;
            if (on_bdry) {
                const Eigen::Vector3d val = g * p;
                for (int a = 0; a < 3; ++a) {
                    fixed[3 * m + a] = val[a];
                }
            }
        } else if (modes_on_boundary(nodes, model.nodes, 0.0, 1.0)) {
            for (int a = 0; a < 3; ++a) {
                fixed[3 * m + a] = 0.0; // linear field has no higher-mode content
            }
        }
    }

    const Eigen::VectorXd loads = Eigen::VectorXd::Zero(sys.ndof);
    const Eigen::VectorXd u = solve_hp(sys, loads, fixed);

    // Every vertex displacement must equal the linear field.
    double max_err = 0.0;
    for (Eigen::Index v = 0; v < static_cast<Eigen::Index>(model.nodes.size()); ++v) {
        const Eigen::Vector3d exact = g * model.nodes[static_cast<std::size_t>(v)];
        const Eigen::Vector3d fem = u.segment<3>(3 * v);
        max_err = std::max(max_err, (fem - exact).norm());
    }
    INFO("max vertex error " << max_err);
    CHECK(max_err < 1e-11);

    // Constant-strain energy error must vanish.
    const Eigen::Matrix<double, 6, 1> eps_const =
        (Eigen::Matrix<double, 6, 1>() << g(0, 0), g(1, 1), g(2, 2), g(1, 2) + g(2, 1),
         g(0, 2) + g(2, 0), g(0, 1) + g(1, 0))
            .finished();
    const double e_energy = hp_energy_error(
        model, sys, u, [&](const Eigen::Vector3d&) { return eps_const; }, kSteel);
    CHECK(e_energy < 1e-6);
}

TEST_CASE("minimum rule keeps mixed p2/p3 order conforming (constant strain)",
          "[hierarchical][hp]") {
    const auto mesh = box_hex_mesh(2, 1, 1, Eigen::Vector3d(1.0, 1.0, 1.0));
    HpModel model = to_hp(mesh, 2);
    REQUIRE(model.elements.size() == 2);
    model.elements[0].order = 3;

    const auto sys = assemble_hp(model, kSteel);

    Eigen::Matrix3d g;
    g << 1e-3, 4e-4, -2e-4, 3e-4, -8e-4, 5e-4, -6e-4, 2e-4, 7e-4;

    std::map<Eigen::Index, double> fixed;
    for (Eigen::Index m = 0; m < sys.n_modes; ++m) {
        const auto& nodes = sys.mode_nodes[static_cast<std::size_t>(m)];
        if (nodes.size() == 1) {
            const Eigen::Vector3d& p = model.nodes[nodes[0]];
            const bool on_bdry = p.x() < 1e-9 || p.x() > 1 - 1e-9 || p.y() < 1e-9 ||
                                 p.y() > 1 - 1e-9 || p.z() < 1e-9 || p.z() > 1 - 1e-9;
            if (on_bdry) {
                const Eigen::Vector3d val = g * p;
                for (int a = 0; a < 3; ++a) {
                    fixed[3 * m + a] = val[a];
                }
            }
        } else if (modes_on_boundary(nodes, model.nodes, 0.0, 1.0)) {
            for (int a = 0; a < 3; ++a) {
                fixed[3 * m + a] = 0.0;
            }
        }
    }

    const Eigen::VectorXd loads = Eigen::VectorXd::Zero(sys.ndof);
    const Eigen::VectorXd u = solve_hp(sys, loads, fixed);

    double max_err = 0.0;
    for (Eigen::Index v = 0; v < static_cast<Eigen::Index>(model.nodes.size()); ++v) {
        const Eigen::Vector3d exact = g * model.nodes[static_cast<std::size_t>(v)];
        const Eigen::Vector3d fem = u.segment<3>(3 * v);
        max_err = std::max(max_err, (fem - exact).norm());
    }
    INFO("max vertex error " << max_err);
    CHECK(max_err < 1e-11);
}

TEST_CASE("hierarchical MMS converges at the theoretical energy-norm rate",
          "[hierarchical][hp]") {
    // Manufactured u_x = sin(pi x) sin(pi y) sin(pi z), u_y = u_z = 0 on the
    // unit cube: vanishes on the whole boundary, so Dirichlet is homogeneous
    // and no boundary-mode projection is needed. Body force is div sigma.
    const double pi = std::numbers::pi;
    const double lam = kSteel.lambda();
    const double mu = kSteel.mu();

    auto body_force = [&](const Eigen::Vector3d& p) {
        const double sx = std::sin(pi * p.x()), sy = std::sin(pi * p.y()),
                     sz = std::sin(pi * p.z());
        const double cx = std::cos(pi * p.x()), cy = std::cos(pi * p.y()),
                     cz = std::cos(pi * p.z());
        Eigen::Vector3d f;
        f.x() = pi * pi * (lam + 4.0 * mu) * sx * sy * sz;
        f.y() = -pi * pi * (lam + mu) * cx * cy * sz;
        f.z() = -pi * pi * (lam + mu) * cx * sy * cz;
        return f;
    };
    auto exact_strain = [&](const Eigen::Vector3d& p) {
        const double sx = std::sin(pi * p.x()), sy = std::sin(pi * p.y()),
                     sz = std::sin(pi * p.z());
        const double cx = std::cos(pi * p.x()), cy = std::cos(pi * p.y()),
                     cz = std::cos(pi * p.z());
        Eigen::Matrix<double, 6, 1> e;
        e << pi * cx * sy * sz, 0.0, 0.0, 0.0, pi * sx * sy * cz, pi * sx * cy * sz;
        return e;
    };

    auto energy_at = [&](std::uint8_t order, int n) {
        auto nodal = box_hex_mesh(n, n, n, Eigen::Vector3d(1.0, 1.0, 1.0));
        const HpModel model = to_hp(nodal, order);
        const auto sys = assemble_hp(model, kSteel);
        const auto fixed = homogeneous_boundary(model, sys);
        const Eigen::VectorXd loads = assemble_hp_body_load(model, sys, body_force);
        const Eigen::VectorXd u = solve_hp(sys, loads, fixed);
        return hp_energy_error(model, sys, u, exact_strain, kSteel);
    };

    SECTION("p = 1 converges at order ~1") {
        const double e_coarse = energy_at(1, 4);
        const double e_fine = energy_at(1, 8);
        const double rate = std::log(e_coarse / e_fine) / std::log(2.0);
        INFO("p1 errors " << e_coarse << " -> " << e_fine << " rate " << rate);
        CHECK(e_fine < e_coarse);
        CHECK(rate > 0.8);
        CHECK(rate < 1.5);
    }

    SECTION("p = 2 converges at order ~2") {
        const double e_coarse = energy_at(2, 4);
        const double e_fine = energy_at(2, 8);
        const double rate = std::log(e_coarse / e_fine) / std::log(2.0);
        INFO("p2 errors " << e_coarse << " -> " << e_fine << " rate " << rate);
        CHECK(e_fine < e_coarse);
        CHECK(rate > 1.7);
        CHECK(rate < 2.5);
    }

    SECTION("p = 3 converges at order ~3") {
        // Finer pair would be expensive; n=2 -> n=4 still exposes the rate.
        const double e_coarse = energy_at(3, 2);
        const double e_fine = energy_at(3, 4);
        const double rate = std::log(e_coarse / e_fine) / std::log(2.0);
        INFO("p3 errors " << e_coarse << " -> " << e_fine << " rate " << rate);
        CHECK(e_fine < e_coarse);
        CHECK(rate > 2.5);
        CHECK(rate < 3.8);
    }

    SECTION("p = 4 converges at order ~4") {
        const double e_coarse = energy_at(4, 2);
        const double e_fine = energy_at(4, 4);
        const double rate = std::log(e_coarse / e_fine) / std::log(2.0);
        INFO("p4 errors " << e_coarse << " -> " << e_fine << " rate " << rate);
        CHECK(e_fine < e_coarse);
        CHECK(rate > 3.3);
        CHECK(rate < 5.0);
    }
}

TEST_CASE("p>=3 hex multi-mode assembly solves the Q2 polynomial MMS exactly",
          "[hierarchical][hp]") {
    // u = (phi,0,0) with phi = x(1-x)y(1-y)z(1-z) lives in Q2. At p>=2 the
    // energy error must vanish (proves multi-mode edge/face/interior DOF
    // numbering + hex face orientation are consistent across elements).
    const double lam = kSteel.lambda();
    const double mu = kSteel.mu();
    auto phi_xx = [](double /*x*/, double y, double z) {
        return -2.0 * y * (1.0 - y) * z * (1.0 - z);
    };
    auto phi_yy = [](double x, double /*y*/, double z) {
        return x * (1.0 - x) * -2.0 * z * (1.0 - z);
    };
    auto phi_zz = [](double x, double y, double /*z*/) {
        return x * (1.0 - x) * y * (1.0 - y) * -2.0;
    };
    auto phi_xy = [](double x, double y, double z) {
        return (1.0 - 2.0 * x) * (1.0 - 2.0 * y) * z * (1.0 - z);
    };
    auto phi_xz = [](double x, double y, double z) {
        return (1.0 - 2.0 * x) * y * (1.0 - y) * (1.0 - 2.0 * z);
    };
    auto dphidx = [](double x, double y, double z) {
        return (1.0 - 2.0 * x) * y * (1.0 - y) * z * (1.0 - z);
    };
    auto dphidy = [](double x, double y, double z) {
        return x * (1.0 - x) * (1.0 - 2.0 * y) * z * (1.0 - z);
    };
    auto dphidz = [](double x, double y, double z) {
        return x * (1.0 - x) * y * (1.0 - y) * (1.0 - 2.0 * z);
    };
    auto body_force = [&](const Eigen::Vector3d& p) {
        const double x = p.x(), y = p.y(), z = p.z();
        Eigen::Vector3d f;
        f.x() = -((lam + 2.0 * mu) * phi_xx(x, y, z) + mu * phi_yy(x, y, z) +
                  mu * phi_zz(x, y, z));
        f.y() = -((lam + mu) * phi_xy(x, y, z));
        f.z() = -((lam + mu) * phi_xz(x, y, z));
        return f;
    };
    auto exact_strain = [&](const Eigen::Vector3d& p) {
        Eigen::Matrix<double, 6, 1> e;
        e << dphidx(p.x(), p.y(), p.z()), 0.0, 0.0, 0.0, dphidz(p.x(), p.y(), p.z()),
            dphidy(p.x(), p.y(), p.z());
        return e;
    };

    for (std::uint8_t order : {static_cast<std::uint8_t>(2), static_cast<std::uint8_t>(3),
                               static_cast<std::uint8_t>(4)}) {
        const auto nodal = box_hex_mesh(2, 2, 2, Eigen::Vector3d(1.0, 1.0, 1.0));
        const HpModel model = to_hp(nodal, order);
        const auto sys = assemble_hp(model, kSteel);
        const auto fixed = homogeneous_boundary(model, sys);
        const Eigen::VectorXd loads = assemble_hp_body_load(model, sys, body_force);
        const Eigen::VectorXd u = solve_hp(sys, loads, fixed);
        const double err = hp_energy_error(model, sys, u, exact_strain, kSteel);
        INFO("order " << int(order) << " Q2-poly energy error " << err);
        CHECK(err < 1e-8);
    }
}
