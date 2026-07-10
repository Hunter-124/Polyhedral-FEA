// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/msh.hpp"

#include <format>
#include <fstream>
#include <sstream>
#include <string_view>

namespace polymesh::fea {
namespace {

struct ParsedLine {
    std::vector<std::string> tokens;
};

ParsedLine tokenize(const std::string& line) {
    ParsedLine out;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        out.tokens.push_back(std::move(tok));
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Gmsh element type ids used by the P1 zoo (reference manual, MSH 2.2).
enum class GmshType : int {
    kTri3 = 2,
    kQuad4 = 3,
    kTet4 = 4,
    kHex8 = 5,
    kTri6 = 9,
    kTet10 = 11,
    kQuad8 = 16,
    kHex20 = 17,
};

bool is_volume(GmshType t) {
    return t == GmshType::kTet4 || t == GmshType::kTet10 || t == GmshType::kHex8 ||
           t == GmshType::kHex20;
}

bool is_surface(GmshType t) {
    return t == GmshType::kTri3 || t == GmshType::kTri6 || t == GmshType::kQuad4 ||
           t == GmshType::kQuad8;
}

int gmsh_num_nodes(GmshType t) {
    switch (t) {
    case GmshType::kTri3:
        return 3;
    case GmshType::kQuad4:
        return 4;
    case GmshType::kTet4:
        return 4;
    case GmshType::kHex8:
        return 8;
    case GmshType::kTri6:
        return 6;
    case GmshType::kTet10:
        return 10;
    case GmshType::kQuad8:
        return 8;
    case GmshType::kHex20:
        return 20;
    }
    return 0;
}

ElementType to_element_type(GmshType t) {
    switch (t) {
    case GmshType::kTet4:
        return ElementType::kTet4;
    case GmshType::kTet10:
        return ElementType::kTet10;
    case GmshType::kHex8:
        return ElementType::kHex8;
    case GmshType::kHex20:
        return ElementType::kHex20;
    default:
        throw FeaError(std::format("msh: not a volume element type {}", static_cast<int>(t)));
    }
}

FaceType to_face_type(GmshType t) {
    switch (t) {
    case GmshType::kTri3:
        return FaceType::kTri3;
    case GmshType::kTri6:
        return FaceType::kTri6;
    case GmshType::kQuad4:
        return FaceType::kQuad4;
    case GmshType::kQuad8:
        return FaceType::kQuad8;
    default:
        throw FeaError(std::format("msh: not a surface element type {}", static_cast<int>(t)));
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw FeaError(std::format("msh: cannot open '{}'", path.string()));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

MshModel parse_msh(const std::string& text) {
    std::istringstream in(text);
    std::string line;

    auto require_section = [&](std::string_view name) {
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            // Trim trailing CR (Windows-authored files).
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line == name) {
                return;
            }
            throw FeaError(std::format("msh: expected section '{}', got '{}'", name, line));
        }
        throw FeaError(std::format("msh: missing section '{}'", name));
    };

    auto next_content_line = [&]() -> std::string {
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#') {
                continue;
            }
            return line;
        }
        throw FeaError("msh: unexpected end of file");
    };

    // --- $MeshFormat ---
    require_section("$MeshFormat");
    {
        const auto fmt = tokenize(next_content_line());
        if (fmt.tokens.size() < 3) {
            throw FeaError("msh: malformed $MeshFormat line");
        }
        const double version = std::stod(fmt.tokens[0]);
        const int file_type = std::stoi(fmt.tokens[1]);
        if (version < 2.0 || version >= 3.0) {
            throw FeaError(std::format(
                "msh: only ASCII format 2.x is supported (got version {})", version));
        }
        if (file_type != 0) {
            throw FeaError("msh: only ASCII (file-type 0) meshes are supported");
        }
    }
    require_section("$EndMeshFormat");

    MshModel model;

    // Optional sections before $Nodes: $PhysicalNames, $Entities (v4 only — reject).
    // Scan until $Nodes.
    while (true) {
        const std::string header = next_content_line();
        if (header == "$Nodes") {
            break;
        }
        if (header == "$PhysicalNames") {
            const int n = std::stoi(next_content_line());
            for (int i = 0; i < n; ++i) {
                const auto pl = tokenize(next_content_line());
                // dimension tag "name"
                if (pl.tokens.size() < 3) {
                    throw FeaError("msh: malformed $PhysicalNames entry");
                }
                const int tag = std::stoi(pl.tokens[1]);
                std::string name = pl.tokens[2];
                // Strip surrounding quotes if present.
                if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
                    name = name.substr(1, name.size() - 2);
                }
                model.physical_names[tag] = std::move(name);
            }
            require_section("$EndPhysicalNames");
            continue;
        }
        if (starts_with(header, "$")) {
            // Skip unknown optional sections of the form $Foo ... $EndFoo.
            if (header == "$Elements") {
                throw FeaError("msh: $Elements before $Nodes");
            }
            const std::string end = "$End" + header.substr(1);
            while (next_content_line() != end) {
            }
            continue;
        }
        throw FeaError(std::format("msh: unexpected content before $Nodes: '{}'", header));
    }

    // --- $Nodes ---
    // Gmsh node numbers need not be contiguous or 1-based dense; build a map.
    const int num_nodes = std::stoi(next_content_line());
    if (num_nodes <= 0) {
        throw FeaError("msh: empty node set");
    }
    std::map<std::int64_t, std::uint32_t> gmsh_to_local;
    model.mesh.nodes.reserve(static_cast<std::size_t>(num_nodes));
    for (int i = 0; i < num_nodes; ++i) {
        const auto nl = tokenize(next_content_line());
        if (nl.tokens.size() < 4) {
            throw FeaError("msh: malformed node line");
        }
        const auto gid = static_cast<std::int64_t>(std::stoll(nl.tokens[0]));
        const double x = std::stod(nl.tokens[1]);
        const double y = std::stod(nl.tokens[2]);
        const double z = std::stod(nl.tokens[3]);
        if (gmsh_to_local.contains(gid)) {
            throw FeaError(std::format("msh: duplicate node id {}", gid));
        }
        gmsh_to_local[gid] = static_cast<std::uint32_t>(model.mesh.nodes.size());
        model.mesh.nodes.emplace_back(x, y, z);
    }
    require_section("$EndNodes");

    // --- $Elements ---
    require_section("$Elements");
    const int num_elements = std::stoi(next_content_line());
    int volume_count = 0;
    for (int e = 0; e < num_elements; ++e) {
        const auto el = tokenize(next_content_line());
        if (el.tokens.size() < 3) {
            throw FeaError("msh: malformed element line");
        }
        // elm-number elm-type number-of-tags <tags...> node-list
        const int type_id = std::stoi(el.tokens[1]);
        const int ntags = std::stoi(el.tokens[2]);
        if (ntags < 0 || static_cast<int>(el.tokens.size()) < 3 + ntags) {
            throw FeaError("msh: element tag count exceeds line tokens");
        }
        const auto gtype = static_cast<GmshType>(type_id);
        const int n_nodes = gmsh_num_nodes(gtype);
        if (n_nodes == 0) {
            // Silently skip unsupported types (lines, points, higher-order, etc.)
            // so mixed CAD-export meshes still load their volume cells.
            continue;
        }
        if (static_cast<int>(el.tokens.size()) < 3 + ntags + n_nodes) {
            throw FeaError(
                std::format("msh: element {} has too few node indices", el.tokens[0]));
        }
        int physical = 0;
        if (ntags >= 1) {
            physical = std::stoi(el.tokens[3]);
        }
        std::vector<std::uint32_t> nodes;
        nodes.reserve(static_cast<std::size_t>(n_nodes));
        for (int k = 0; k < n_nodes; ++k) {
            const auto gid = static_cast<std::int64_t>(
                std::stoll(el.tokens[static_cast<std::size_t>(3 + ntags + k)]));
            const auto it = gmsh_to_local.find(gid);
            if (it == gmsh_to_local.end()) {
                throw FeaError(std::format("msh: element references unknown node {}", gid));
            }
            nodes.push_back(it->second);
        }
        if (is_volume(gtype)) {
            model.mesh.elements.push_back({to_element_type(gtype), std::move(nodes)});
            ++volume_count;
        } else if (is_surface(gtype)) {
            model.physical_faces[physical].push_back({to_face_type(gtype), std::move(nodes)});
        }
    }
    require_section("$EndElements");

    if (volume_count == 0) {
        throw FeaError("msh: no supported volume elements (tet4/10, hex8/20) found");
    }
    model.mesh.check_validity();
    return model;
}

MshModel load_msh(const std::filesystem::path& path) { return parse_msh(read_file(path)); }

} // namespace polymesh::fea
