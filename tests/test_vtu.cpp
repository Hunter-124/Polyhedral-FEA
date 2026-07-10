// SPDX-License-Identifier: BSD-3-Clause
#include "fea/vtu.hpp"
#include "mesh/tet_fill.hpp"
#include "support/structured_mesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path unique_temp_vtu(const char* prefix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "_" + std::to_string(stamp) + ".vtu");
}

} // namespace

TEST_CASE("write_vtu emits VTK unstructured grid header") {
    auto mesh = polymesh::test_support::box_hex_mesh(2, 2, 2, {1.0, 1.0, 1.0});
    // Unique path + close before remove: fixed names race under ctest -j on Windows.
    const auto path = unique_temp_vtu("polymesh_test");
    polymesh::fea::write_vtu(path, mesh);
    {
        std::ifstream in(path);
        REQUIRE(in);
        std::string head(200, '\0');
        in.read(head.data(), 200);
        head.resize(static_cast<std::size_t>(in.gcount()));
        REQUIRE(head.find("UnstructuredGrid") != std::string::npos);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("write_vtu CellData quality for tet4 mesh") {
    polymesh::geom::TriSurface s;
    s.vertices = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0},
                  {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    s.triangles = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                   {2, 3, 7}, {2, 7, 6}, {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5}};
    auto fill = polymesh::mesh::tet_fill_surface(s, {0, 0, 0}, {1, 1, 1}, 0.5, true);
    REQUIRE_FALSE(fill.tets.empty());

    polymesh::fea::NodalMesh mesh;
    mesh.nodes = fill.nodes;
    for (const auto& t : fill.tets) {
        mesh.elements.push_back(polymesh::fea::NodalElement{polymesh::fea::ElementType::kTet4,
                                                            {t[0], t[1], t[2], t[3]}});
    }

    const auto quality = polymesh::fea::tet4_cell_quality(mesh);
    REQUIRE(quality.size() == mesh.elements.size());
    REQUIRE(quality[0] > 0.0);

    const auto path = unique_temp_vtu("polymesh_quality");
    std::vector<polymesh::fea::VtuCellData> cdata;
    cdata.push_back({.name = "quality", .scalars = quality});
    polymesh::fea::write_vtu(path, mesh, {}, cdata);

    {
        std::ifstream in(path);
        REQUIRE(in);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string xml = ss.str();
        REQUIRE(xml.find("<CellData>") != std::string::npos);
        REQUIRE(xml.find("Name=\"quality\"") != std::string::npos);
        REQUIRE(xml.find("</CellData>") != std::string::npos);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
