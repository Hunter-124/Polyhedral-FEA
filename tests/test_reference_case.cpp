// SPDX-License-Identifier: AGPL-3.0-or-later
#include "bench/reference_case.hpp"

#include <catch2/catch_test_macros.hpp>

using polymesh::bench::BenchError;
using polymesh::bench::parse_reference;

TEST_CASE("parses reference case json") {
    const auto c = parse_reference(R"({
        "name": "kirsch-plate",
        "citation": "Kirsch 1898; SCF = 3 for uniaxial tension, infinite plate, circular hole",
        "values": { "scf": 3.0 }
    })");
    CHECK(c.name == "kirsch-plate");
    CHECK(c.values.at("scf") == 3.0);
}

TEST_CASE("missing fields are a BenchError") {
    REQUIRE_THROWS_AS(parse_reference(R"({"name": "x"})"), BenchError);
}
