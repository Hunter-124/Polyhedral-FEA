// SPDX-License-Identifier: AGPL-3.0-or-later
#include "geom/stl.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace detail = polymesh::geom::detail;

namespace {

std::vector<std::byte> as_bytes(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

constexpr std::string_view kTetraAscii = R"(solid tetra
 facet normal 0 0 -1
  outer loop
   vertex 0 0 0
   vertex 0 1 0
   vertex 1 0 0
  endloop
 endfacet
 facet normal 0 -1 0
  outer loop
   vertex 0 0 0
   vertex 1 0 0
   vertex 0 0 1
  endloop
 endfacet
 facet normal -1 0 0
  outer loop
   vertex 0 0 0
   vertex 0 0 1
   vertex 0 1 0
  endloop
 endfacet
 facet normal 1 1 1
  outer loop
   vertex 1 0 0
   vertex 0 1 0
   vertex 0 0 1
  endloop
 endfacet
endsolid tetra
)";

} // namespace

TEST_CASE("ascii tetra welds to four vertices") {
    const auto soup = detail::parse_ascii(as_bytes(kTetraAscii));
    REQUIRE(soup.size() == 4);
    const auto surface = detail::weld(soup);
    CHECK(surface.vertices.size() == 4);
    CHECK(surface.triangles.size() == 4);
    REQUIRE_NOTHROW(surface.validate());
}

TEST_CASE("binary roundtrip") {
    // Build a binary STL for one triangle and parse it back.
    std::vector<std::byte> bytes(84, std::byte{0});
    const std::uint32_t count = 1;
    std::memcpy(bytes.data() + 80, &count, 4);
    std::vector<std::byte> record(12, std::byte{0}); // normal
    for (const float v : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        std::byte le[4];
        std::memcpy(le, &v, 4);
        record.insert(record.end(), le, le + 4);
    }
    record.insert(record.end(), 2, std::byte{0}); // attribute byte count
    bytes.insert(bytes.end(), record.begin(), record.end());

    const auto soup = detail::parse_binary(bytes);
    REQUIRE(soup.size() == 1);
    CHECK(soup[0][3] == 1.0);
}

TEST_CASE("truncated binary header is rejected") {
    const std::vector<std::byte> bytes(40, std::byte{0});
    REQUIRE_THROWS_AS(detail::parse_binary(bytes), polymesh::geom::GeomError);
}
