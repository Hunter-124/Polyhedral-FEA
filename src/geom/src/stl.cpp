// SPDX-License-Identifier: BSD-3-Clause
#include "geom/stl.hpp"

#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <sstream>

namespace polymesh::geom {
namespace detail {
namespace {

// STL files are little-endian; supporting big-endian hosts would need
// byte-swapping here.
static_assert(std::endian::native == std::endian::little);

/// Reads a little-endian scalar at byte offset `at`.
template <typename T> T read_le(std::span<const std::byte> bytes, std::size_t at) {
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    return value;
}

bool is_ascii_stl(std::span<const std::byte> bytes) {
    // A binary STL can also start with "solid"; require an ASCII "facet"
    // token near the top to disambiguate.
    constexpr std::string_view kSolid = "solid";
    if (bytes.size() < kSolid.size() ||
        std::memcmp(bytes.data(), kSolid.data(), kSolid.size()) != 0) {
        return false;
    }
    const std::string_view head(reinterpret_cast<const char*>(bytes.data()),
                                std::min<std::size_t>(bytes.size(), 1024));
    return head.find("facet") != std::string_view::npos;
}

} // namespace

Soup parse_binary(std::span<const std::byte> bytes) {
    constexpr std::size_t kHeader = 84;
    constexpr std::size_t kRecord = 50;
    if (bytes.size() < kHeader) {
        throw GeomError("malformed STL: shorter than 84-byte header");
    }
    const auto n = read_le<std::uint32_t>(bytes, 80);
    const std::size_t expected = kHeader + std::size_t{n} * kRecord;
    if (bytes.size() < expected) {
        throw GeomError(std::format(
            "malformed STL: header declares {} triangles ({} bytes) but file has {}", n,
            expected, bytes.size()));
    }
    Soup soup;
    soup.reserve(n);
    for (std::size_t t = 0; t < n; ++t) {
        // Skip the 12-byte stored normal; it is recomputed from vertices.
        const std::size_t base = kHeader + t * kRecord + 12;
        std::array<double, 9> tri{};
        for (std::size_t k = 0; k < 9; ++k) {
            tri[k] = static_cast<double>(read_le<float>(bytes, base + k * 4));
        }
        soup.push_back(tri);
    }
    return soup;
}

Soup parse_ascii(std::span<const std::byte> bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Soup soup;
    std::array<double, 9> coords{};
    std::size_t filled = 0;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string keyword;
        words >> keyword;
        if (keyword != "vertex") {
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            double value = 0.0;
            if (!(words >> value)) {
                throw GeomError("malformed STL: vertex with fewer than 3 coordinates");
            }
            coords[filled++] = value;
        }
        if (filled == 9) {
            soup.push_back(coords);
            filled = 0;
        }
    }
    if (filled != 0) {
        throw GeomError("malformed STL: facet with incomplete vertex triple");
    }
    return soup;
}

TriSurface weld(const Soup& soup) {
    // Key on the raw bit patterns for exact, tolerance-free deduplication.
    std::map<std::array<std::uint64_t, 3>, std::uint32_t> seen;
    TriSurface surface;
    for (const auto& tri : soup) {
        std::array<std::uint32_t, 3> idx{};
        for (std::size_t v = 0; v < 3; ++v) {
            const std::array<double, 3> p{tri[v * 3], tri[v * 3 + 1], tri[v * 3 + 2]};
            const std::array<std::uint64_t, 3> key{std::bit_cast<std::uint64_t>(p[0]),
                                                   std::bit_cast<std::uint64_t>(p[1]),
                                                   std::bit_cast<std::uint64_t>(p[2])};
            const auto [it, inserted] =
                seen.try_emplace(key, static_cast<std::uint32_t>(surface.vertices.size()));
            if (inserted) {
                surface.vertices.emplace_back(p[0], p[1], p[2]);
            }
            idx[v] = it->second;
        }
        surface.triangles.push_back(idx);
    }
    return surface;
}

} // namespace detail

TriSurface load_stl(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw GeomError(std::format("cannot open {}", path.string()));
    }
    std::vector<std::byte> bytes(std::filesystem::file_size(path));
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw GeomError(std::format("i/o error reading {}", path.string()));
    }
    const auto soup =
        detail::is_ascii_stl(bytes) ? detail::parse_ascii(bytes) : detail::parse_binary(bytes);
    return detail::weld(soup);
}

} // namespace polymesh::geom
