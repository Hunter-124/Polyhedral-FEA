// SPDX-License-Identifier: BSD-3-Clause
#include "support/structured_mesh.hpp"

#include "fea/p_elevate.hpp"

#include <array>

namespace polymesh::test_support {

using fea::ElementType;
using fea::NodalElement;
using fea::NodalMesh;

namespace {

/// Grid of (nx+1)(ny+1)(nz+1) nodes; returns node index for lattice (i,j,k).
std::uint32_t grid_node(int i, int j, int k, int nx, int ny) {
    return static_cast<std::uint32_t>((k * (ny + 1) + j) * (nx + 1) + i);
}

NodalMesh grid_nodes(int nx, int ny, int nz, const Eigen::Vector3d& size) {
    NodalMesh mesh;
    mesh.nodes.reserve(static_cast<std::size_t>((nx + 1) * (ny + 1) * (nz + 1)));
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                mesh.nodes.emplace_back(size[0] * i / nx, size[1] * j / ny, size[2] * k / nz);
            }
        }
    }
    return mesh;
}

/// Hex corner nodes of cell (i,j,k) in canonical hex8 order.
std::array<std::uint32_t, 8> cell_corners(int i, int j, int k, int nx, int ny) {
    return {grid_node(i, j, k, nx, ny),
            grid_node(i + 1, j, k, nx, ny),
            grid_node(i + 1, j + 1, k, nx, ny),
            grid_node(i, j + 1, k, nx, ny),
            grid_node(i, j, k + 1, nx, ny),
            grid_node(i + 1, j, k + 1, nx, ny),
            grid_node(i + 1, j + 1, k + 1, nx, ny),
            grid_node(i, j + 1, k + 1, nx, ny)};
}

} // namespace

NodalMesh box_hex_mesh(int nx, int ny, int nz, const Eigen::Vector3d& size) {
    NodalMesh mesh = grid_nodes(nx, ny, nz, size);
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto c = cell_corners(i, j, k, nx, ny);
                mesh.elements.push_back(
                    NodalElement{ElementType::kHex8, {c.begin(), c.end()}});
            }
        }
    }
    return mesh;
}

NodalMesh box_tet_mesh(int nx, int ny, int nz, const Eigen::Vector3d& size) {
    NodalMesh mesh = grid_nodes(nx, ny, nz, size);
    // Kuhn triangulation: six tets around the main diagonal (corner 0 to 6);
    // conforming across translated cells.
    constexpr std::array<std::array<int, 4>, 6> kTets{
        {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6}, {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}}};
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const auto c = cell_corners(i, j, k, nx, ny);
                for (const auto& t : kTets) {
                    mesh.elements.push_back(NodalElement{ElementType::kTet4,
                                                         {c[static_cast<std::size_t>(t[0])],
                                                          c[static_cast<std::size_t>(t[1])],
                                                          c[static_cast<std::size_t>(t[2])],
                                                          c[static_cast<std::size_t>(t[3])]}});
                }
            }
        }
    }
    return mesh;
}

NodalMesh promote_to_quadratic(const NodalMesh& mesh) {
    return fea::promote_to_quadratic(mesh);
}

void distort_interior(NodalMesh& mesh, double amplitude, double h, std::uint64_t seed) {
    Eigen::Vector3d lo = mesh.nodes.front();
    Eigen::Vector3d hi = mesh.nodes.front();
    for (const auto& p : mesh.nodes) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    const double tol = 1e-12 * (hi - lo).norm();
    std::uint64_t state = seed;
    const auto next_offset = [&] {
        // xorshift64*: deterministic, seed-controlled (engineering rule 5).
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const auto bits = state * 0x2545F4914F6CDD1Dull;
        // Map to [-1, 1).
        return 2.0 * (static_cast<double>(bits >> 11) * 0x1.0p-53) - 1.0;
    };
    for (auto& p : mesh.nodes) {
        bool boundary = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (p[axis] < lo[axis] + tol || p[axis] > hi[axis] - tol) {
                boundary = true;
            }
        }
        // Draw the offsets regardless so the perturbation pattern of a node
        // doesn't depend on which other nodes are boundary.
        const Eigen::Vector3d offset(next_offset(), next_offset(), next_offset());
        if (!boundary) {
            p += amplitude * h * offset;
        }
    }
}

} // namespace polymesh::test_support
