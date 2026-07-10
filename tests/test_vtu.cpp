// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vtu.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("write_vtu emits VTK unstructured grid header") {
    auto mesh = polymesh::test_support::box_hex_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    const auto path = std::filesystem::temp_directory_path() / "polymesh_test.vtu";
    polymesh::fea::write_vtu(path, mesh);
    std::ifstream in(path);
    REQUIRE(in);
    std::string head(200, '\0');
    in.read(head.data(), 200);
    head.resize(static_cast<std::size_t>(in.gcount()));
    REQUIRE(head.find("UnstructuredGrid") != std::string::npos);
    std::filesystem::remove(path);
}
