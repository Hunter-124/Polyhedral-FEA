// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Binary and ASCII STL loading with vertex welding.

#include "geom/tri_surface.hpp"

#include <filesystem>
#include <span>

namespace polymesh::geom {

/// Raw triangle soup: 9 coordinates (3 vertices) per triangle, metres.
using Soup = std::vector<std::array<double, 9>>;

/// Loads an STL file (binary or ASCII, auto-detected) and welds duplicate
/// vertices exactly (bitwise equality — STL repeats vertices verbatim, so
/// exact welding is deterministic and tolerance-free).
/// Throws GeomError on malformed input or I/O failure.
TriSurface load_stl(const std::filesystem::path& path);

// Exposed for unit testing; prefer load_stl().
namespace detail {
Soup parse_binary(std::span<const std::byte> bytes);
Soup parse_ascii(std::span<const std::byte> bytes);
TriSurface weld(const Soup& soup);
} // namespace detail

} // namespace polymesh::geom
