// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Mirror-symmetric tetrahedral decomposition of a Cartesian lattice cell.
//
// Kuhn's six-tet cell with its main diagonal rotated per cell, so a
// mirror-symmetric part on a mirror-symmetric lattice gets a mirror-symmetric
// tiling. The six-tet cell is kept over the better-shaped five-tet
// ("checkerboard BCC") split because its unique longest edge keeps
// longest-edge bisection cell-local (ADR-0036).

#include <array>
#include <cstddef>

namespace polymesh::mesh {

/// Alternating **Kuhn** split — the six-tet decomposition kept, but the cube's
/// main diagonal rotated per cell so the tiling gains the mirror symmetry the
/// single-orientation Kuhn lattice never had.
///
/// A Kuhn cell is the six coordinate-order paths from a low corner `a` to the
/// opposite corner `b`; every tet contains the segment a-b, which is also its
/// unique longest edge (√3·h against √2·h and h). That is why longest-edge
/// bisection on this lattice stays *cell-local*: the first terminal edge of any
/// marked cell is the cell's own main diagonal, shared by nothing outside it.
///
/// Conformity fixes which diagonals may sit side by side. On a shared face both
/// cells must pick the same face diagonal, and a Kuhn cell's diagonal on each
/// face is the projection of a-b. Writing that direction as signs (s1,s2,s3)
/// modulo global negation, the x-face is fixed by s2·s3, the y-face by s1·s3 and
/// the z-face by s1·s2 — so s2·s3 must be constant along x, s1·s3 along y and
/// s1·s2 along z. The mirror-antisymmetric solution of that system is
/// s = ((-1)^(i+j), 1, (-1)^(j+k)): reflecting an axis negates the corresponding
/// sign, and with even cell counts the index parity negates it too, so the
/// reflected cell receives exactly the reflected variant.
///
/// Variant 0 is the single-orientation table (main diagonal corner 0 to
/// corner 6), used by cells whose index sums are both even.
inline constexpr std::array<std::array<std::array<int, 4>, 6>, 4> kLatticeTetsKuhn{{
    // variant 0: main diagonal (0,0,0)-(1,1,1)
    {{{{0, 1, 2, 6}},
      {{0, 1, 5, 6}},
      {{0, 3, 2, 6}},
      {{0, 3, 7, 6}},
      {{0, 4, 5, 6}},
      {{0, 4, 7, 6}}}},
    // variant 1: main diagonal (1,0,0)-(0,1,1)
    {{{{1, 0, 3, 7}},
      {{1, 0, 4, 7}},
      {{1, 2, 3, 7}},
      {{1, 2, 6, 7}},
      {{1, 5, 4, 7}},
      {{1, 5, 6, 7}}}},
    // variant 2: main diagonal (0,0,1)-(1,1,0)
    {{{{4, 5, 6, 2}},
      {{4, 5, 1, 2}},
      {{4, 7, 6, 2}},
      {{4, 7, 3, 2}},
      {{4, 0, 1, 2}},
      {{4, 0, 3, 2}}}},
    // variant 3: main diagonal (1,0,1)-(0,1,0)
    {{{{5, 4, 7, 3}},
      {{5, 4, 0, 3}},
      {{5, 6, 7, 3}},
      {{5, 6, 2, 3}},
      {{5, 1, 0, 3}},
      {{5, 1, 2, 3}}}},
}};

/// Which `kLatticeTetsKuhn` variant cell (i,j,k) uses.
[[nodiscard]] inline constexpr std::size_t lattice_cell_variant(int i, int j, int k) {
    return static_cast<std::size_t>(((i + j) & 1) | (((j + k) & 1) << 1));
}

} // namespace polymesh::mesh
