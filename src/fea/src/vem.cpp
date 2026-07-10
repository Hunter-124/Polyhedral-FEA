// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vem.hpp"

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <cmath>
#include <format>
#include <stdexcept>

namespace polymesh::fea {
namespace {

Eigen::Vector3d face_normal_area(const std::vector<Eigen::Vector3d>& coords,
                                 const std::vector<std::uint32_t>& face, double& area_out) {
    // Newell: robust for non-planar quads.
    Eigen::Vector3d n = Eigen::Vector3d::Zero();
    const auto m = face.size();
    for (std::size_t i = 0; i < m; ++i) {
        const auto& a = coords[face[i]];
        const auto& b = coords[face[(i + 1) % m]];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    n *= 0.5;
    area_out = n.norm();
    if (area_out <= 0.0) {
        return Eigen::Vector3d::Zero();
    }
    return n; // area-weighted normal (magnitude = area)
}

Eigen::MatrixXd b_from_grads(const Eigen::Matrix<double, Eigen::Dynamic, 3>& dndx) {
    const Eigen::Index n = dndx.rows();
    Eigen::MatrixXd b = Eigen::MatrixXd::Zero(6, 3 * n);
    for (Eigen::Index a = 0; a < n; ++a) {
        const double dx = dndx(a, 0), dy = dndx(a, 1), dz = dndx(a, 2);
        b(0, 3 * a + 0) = dx;
        b(1, 3 * a + 1) = dy;
        b(2, 3 * a + 2) = dz;
        b(3, 3 * a + 1) = dz;
        b(3, 3 * a + 2) = dy;
        b(4, 3 * a + 0) = dz;
        b(4, 3 * a + 2) = dx;
        b(5, 3 * a + 0) = dy;
        b(5, 3 * a + 1) = dx;
    }
    return b;
}

/// Columns = DOF vectors of the 12-dim linear space (RBM + constant strain)
/// evaluated at vertices. Size 3n × 12.
Eigen::MatrixXd linear_space_basis(const std::vector<Eigen::Vector3d>& coords) {
    const auto n = static_cast<Eigen::Index>(coords.size());
    Eigen::MatrixXd p(3 * n, 12);
    p.setZero();
    // Rigid: translations
    for (Eigen::Index i = 0; i < n; ++i) {
        p(3 * i + 0, 0) = 1.0;
        p(3 * i + 1, 1) = 1.0;
        p(3 * i + 2, 2) = 1.0;
        // Rigid: rotations about origin (infinitesimal): ω × x
        const auto& x = coords[static_cast<std::size_t>(i)];
        // ω = e_x → (0, -z, y)
        p(3 * i + 1, 3) = -x[2];
        p(3 * i + 2, 3) = x[1];
        // ω = e_y → (z, 0, -x)
        p(3 * i + 0, 4) = x[2];
        p(3 * i + 2, 4) = -x[0];
        // ω = e_z → (-y, x, 0)
        p(3 * i + 0, 5) = -x[1];
        p(3 * i + 1, 5) = x[0];
        // Constant strain: u = ε x (symmetric ε Voigt: xx,yy,zz,yz,xz,xy)
        // ε_xx
        p(3 * i + 0, 6) = x[0];
        // ε_yy
        p(3 * i + 1, 7) = x[1];
        // ε_zz
        p(3 * i + 2, 8) = x[2];
        // ε_yz (eng shear /2 on each for displacement? use eng: γ_yz = 2ε_yz → u_y+=ε z,
        // u_z+=ε y)
        p(3 * i + 1, 9) = x[2];
        p(3 * i + 2, 9) = x[1];
        // ε_xz
        p(3 * i + 0, 10) = x[2];
        p(3 * i + 2, 10) = x[0];
        // ε_xy
        p(3 * i + 0, 11) = x[1];
        p(3 * i + 1, 11) = x[0];
    }
    return p;
}

} // namespace

double poly_volume(const std::vector<Eigen::Vector3d>& coords,
                   const std::vector<std::vector<std::uint32_t>>& faces) {
    // V = (1/3) sum_f x_f · (n_f * A_f) with x_f face centroid.
    double vol = 0.0;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        double area = 0.0;
        const Eigen::Vector3d nA = face_normal_area(coords, face, area);
        Eigen::Vector3d c = Eigen::Vector3d::Zero();
        for (auto i : face) {
            c += coords[i];
        }
        c /= static_cast<double>(face.size());
        vol += c.dot(nA) / 3.0;
    }
    return vol;
}

Eigen::MatrixXd vem_poly_stiffness(const std::vector<Eigen::Vector3d>& coords,
                                   const std::vector<std::vector<std::uint32_t>>& faces,
                                   const Material& material) {
    const auto n = coords.size();
    if (n < 4) {
        throw FeaError("vem_poly_stiffness: need at least 4 vertices");
    }
    const double vol = poly_volume(coords, faces);
    if (vol <= 0.0) {
        throw FeaError(std::format("vem_poly_stiffness: non-positive volume {:.3e}", vol));
    }

    // Average nodal gradients via ∫_∂E φ_i n dS / V.
    // On a face with m vertices, ∫ φ_i dS = A/m for each vertex of the face
    // (exact for linear φ on a triangle; good approx for planar quads).
    Eigen::Matrix<double, Eigen::Dynamic, 3> grad(static_cast<Eigen::Index>(n), 3);
    grad.setZero();
    double h_char = 0.0;
    for (const auto& face : faces) {
        if (face.size() < 3) {
            continue;
        }
        double area = 0.0;
        const Eigen::Vector3d nA = face_normal_area(coords, face, area);
        if (area <= 0.0) {
            continue;
        }
        h_char += area;
        const double w = 1.0 / static_cast<double>(face.size());
        for (auto li : face) {
            grad.row(static_cast<Eigen::Index>(li)) += (w * nA).transpose();
        }
    }
    grad /= vol;
    h_char = std::sqrt(h_char / (6.0 * vol + 1e-30)); // ~ characteristic length
    if (h_char <= 0.0) {
        h_char = std::cbrt(vol);
    }

    const auto b = b_from_grads(grad);
    const auto d = material.d_matrix();
    Eigen::MatrixXd k = vol * (b.transpose() * d * b);

    // Stabilization: τ μ h (I - Π)ᵀ(I - Π), Π = projector onto linear space.
    const Eigen::MatrixXd basis = linear_space_basis(coords);
    // Orthonormalize columns of basis via thin QR for a stable projector.
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(basis);
    const Eigen::MatrixXd q =
        qr.householderQ() * Eigen::MatrixXd::Identity(basis.rows(), qr.rank());
    const Eigen::MatrixXd proj = q * q.transpose(); // 3n×3n
    const Eigen::Index ndof = 3 * static_cast<Eigen::Index>(n);
    const Eigen::MatrixXd i_minus = Eigen::MatrixXd::Identity(ndof, ndof) - proj;
    const double tau = 1.0; // dimensionless
    const double stab_scale = tau * material.mu() * h_char;
    k.noalias() += stab_scale * (i_minus.transpose() * i_minus);

    // Symmetrize numerical noise.
    k = 0.5 * (k + k.transpose());
    return k;
}

Eigen::MatrixXd vem_poly_stiffness(const NodalMesh& mesh, const PolyCell& cell,
                                   const Material& material) {
    std::vector<Eigen::Vector3d> coords;
    coords.reserve(cell.nodes.size());
    for (auto id : cell.nodes) {
        if (id >= mesh.nodes.size()) {
            throw FeaError("vem_poly_stiffness: node out of range");
        }
        coords.push_back(mesh.nodes[id]);
    }
    return vem_poly_stiffness(coords, cell.faces, material);
}

PolyCell hex8_as_poly(const NodalElement& hex) {
    if (hex.nodes.size() != 8) {
        throw FeaError("hex8_as_poly: expected 8 nodes");
    }
    PolyCell c;
    c.nodes = hex.nodes;
    // Local indices 0..7, outward faces for right-handed hex.
    c.faces = {
        {0, 3, 2, 1}, // bottom -z
        {4, 5, 6, 7}, // top +z
        {0, 1, 5, 4}, // -y
        {2, 3, 7, 6}, // +y
        {0, 4, 7, 3}, // -x
        {1, 2, 6, 5}, // +x
    };
    return c;
}

PolyCell tet4_as_poly(const NodalElement& tet) {
    if (tet.nodes.size() != 4) {
        throw FeaError("tet4_as_poly: expected 4 nodes");
    }
    PolyCell c;
    c.nodes = tet.nodes;
    // Faces with outward orientation relative to tet volume (0,1,2,3) ref.
    c.faces = {
        {0, 2, 1},
        {0, 1, 3},
        {0, 3, 2},
        {1, 2, 3},
    };
    return c;
}

} // namespace polymesh::fea
