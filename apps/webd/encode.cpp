// SPDX-License-Identifier: BSD-3-Clause
#include "encode.hpp"

#include "fea/traction.hpp"
#include "geom/tri_surface.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include <Eigen/Geometry> // cross()

#include <nlohmann/json.hpp>

namespace polymesh::webd {
namespace {

using FaceLoop = std::vector<std::uint32_t>;

struct BoundaryFace {
    FaceLoop loop;
    std::size_t owner = 0;
};

std::size_t corner_count(fea::ElementType type, std::size_t fallback) {
    switch (type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        return 4;
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        return 8;
    case fea::ElementType::kPrism6:
        return 6;
    case fea::ElementType::kPyramid5:
        return 5;
    case fea::ElementType::kPolyVem:
        return fallback;
    }
    return fallback;
}

std::vector<std::vector<std::size_t>> local_faces(const fea::NodalElement& element) {
    switch (element.type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        return {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {2, 0, 3}};
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        return {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4},
                {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    case fea::ElementType::kPrism6:
        return {{0, 2, 1}, {3, 4, 5}, {0, 1, 4, 3}, {1, 2, 5, 4}, {2, 0, 3, 5}};
    case fea::ElementType::kPyramid5:
        return {{0, 3, 2, 1}, {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
    case fea::ElementType::kPolyVem: {
        std::vector<std::vector<std::size_t>> faces;
        faces.reserve(element.faces.size());
        for (const auto& face : element.faces) {
            faces.emplace_back(face.begin(), face.end());
        }
        return faces;
    }
    }
    return {};
}

std::vector<BoundaryFace> boundary_faces(const fea::NodalMesh& mesh) {
    struct SeenFace {
        BoundaryFace face;
        std::size_t count = 0;
    };
    std::map<FaceLoop, SeenFace> seen;
    for (std::size_t owner = 0; owner < mesh.elements.size(); ++owner) {
        const auto& element = mesh.elements[owner];
        for (const auto& local : local_faces(element)) {
            FaceLoop loop;
            loop.reserve(local.size());
            bool valid = local.size() >= 3;
            for (const std::size_t index : local) {
                if (index >= element.nodes.size() ||
                    element.nodes[index] >= mesh.nodes.size()) {
                    valid = false;
                    break;
                }
                loop.push_back(element.nodes[index]);
            }
            if (!valid) {
                continue;
            }
            FaceLoop key = loop;
            std::sort(key.begin(), key.end());
            auto& entry = seen[key];
            if (entry.count == 0) {
                entry.face = {std::move(loop), owner};
            }
            ++entry.count;
        }
    }
    std::vector<BoundaryFace> result;
    result.reserve(seen.size());
    for (auto& [key, entry] : seen) {
        (void)key;
        if (entry.count == 1) {
            result.push_back(std::move(entry.face));
        }
    }
    return result;
}

Eigen::Vector3d element_centroid(const fea::NodalMesh& mesh, std::size_t owner) {
    const auto& element = mesh.elements.at(owner);
    const std::size_t count =
        std::min(corner_count(element.type, element.nodes.size()), element.nodes.size());
    if (count == 0) {
        throw std::runtime_error("cannot find a centroid for an empty element");
    }
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    for (std::size_t index = 0; index < count; ++index) {
        centroid += mesh.nodes.at(element.nodes[index]);
    }
    return centroid / static_cast<double>(count);
}

// Duplicated intentionally from apps/gui/viewport.cpp:436-451. The web server
// does not link the GUI, but a given element type must retain the same identity.
std::array<float, 3> element_color(fea::ElementType type) {
    switch (type) {
    case fea::ElementType::kTet4:
    case fea::ElementType::kTet10:
        return {0.42F, 0.58F, 0.92F};
    case fea::ElementType::kHex8:
    case fea::ElementType::kHex20:
        return {0.35F, 0.78F, 0.50F};
    case fea::ElementType::kPyramid5:
        return {0.95F, 0.58F, 0.28F};
    case fea::ElementType::kPrism6:
        return {0.72F, 0.45F, 0.90F};
    case fea::ElementType::kPolyVem:
        return {0.25F, 0.82F, 0.85F};
    }
    throw std::runtime_error("unknown finite-element type");
}

void append_point(std::vector<float>& output, const Eigen::Vector3d& point) {
    output.push_back(static_cast<float>(point.x()));
    output.push_back(static_cast<float>(point.y()));
    output.push_back(static_cast<float>(point.z()));
}

nlohmann::json arrays_json(std::vector<float>&& positions, std::vector<float>&& centroids,
                           std::vector<float>&& indices, std::vector<float>&& colors,
                           std::vector<float>&& edges) {
    nlohmann::json value;
    value["n_verts"] = positions.size() / 3;
    value["positions_b64"] = base64_f32(positions);
    value["centroids_b64"] = base64_f32(centroids);
    value["index_b64"] = base64_f32(indices);
    value["color_b64"] = base64_f32(colors);
    value["n_edge_verts"] = edges.size() / 3;
    value["edges_b64"] = base64_f32(edges);
    return value;
}

double interpolate_scalar(const fea::SurfaceSample& sample,
                          const std::vector<double>& values) {
    double result = 0.0;
    for (std::size_t index = 0; index < sample.count; ++index) {
        const std::uint32_t node = sample.source_nodes[index];
        if (node >= values.size()) {
            throw std::runtime_error("solve field is shorter than the surface node map");
        }
        result += sample.weights[index] * values[node];
    }
    return result;
}

Eigen::Vector3d interpolate_displacement(const fea::SurfaceSample& sample,
                                         const Eigen::VectorXd& displacement) {
    Eigen::Vector3d result = Eigen::Vector3d::Zero();
    for (std::size_t index = 0; index < sample.count; ++index) {
        const Eigen::Index base = 3 * static_cast<Eigen::Index>(sample.source_nodes[index]);
        if (base + 2 >= displacement.size()) {
            throw std::runtime_error("displacement is shorter than the surface node map");
        }
        result += sample.weights[index] * displacement.segment<3>(base);
    }
    return result;
}

} // namespace

std::string base64_bytes(std::span<const std::byte> bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(4 * ((bytes.size() + 2) / 3));
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const auto a = std::to_integer<std::uint32_t>(bytes[index]);
        const auto b = std::to_integer<std::uint32_t>(bytes[index + 1]);
        const auto c = std::to_integer<std::uint32_t>(bytes[index + 2]);
        const std::uint32_t bits = (a << 16U) | (b << 8U) | c;
        output.push_back(kAlphabet[(bits >> 18U) & 63U]);
        output.push_back(kAlphabet[(bits >> 12U) & 63U]);
        output.push_back(kAlphabet[(bits >> 6U) & 63U]);
        output.push_back(kAlphabet[bits & 63U]);
        index += 3;
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining > 0) {
        const auto a = std::to_integer<std::uint32_t>(bytes[index]);
        const auto b = remaining == 2 ? std::to_integer<std::uint32_t>(bytes[index + 1]) : 0U;
        const std::uint32_t bits = (a << 16U) | (b << 8U);
        output.push_back(kAlphabet[(bits >> 18U) & 63U]);
        output.push_back(kAlphabet[(bits >> 12U) & 63U]);
        output.push_back(remaining == 2 ? kAlphabet[(bits >> 6U) & 63U] : '=');
        output.push_back('=');
    }
    return output;
}

std::string base64_f32(std::span<const float> values) {
    std::vector<std::byte> little_endian;
    little_endian.reserve(values.size() * sizeof(float));
    for (const float value : values) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        little_endian.push_back(static_cast<std::byte>(bits & 0xffU));
        little_endian.push_back(static_cast<std::byte>((bits >> 8U) & 0xffU));
        little_endian.push_back(static_cast<std::byte>((bits >> 16U) & 0xffU));
        little_endian.push_back(static_cast<std::byte>((bits >> 24U) & 0xffU));
    }
    return base64_bytes(little_endian);
}

nlohmann::json part_surface_json(const pipeline::Model& model) {
    if (model.triangle_region.size() != model.surface.triangles.size()) {
        throw std::runtime_error("model triangle regions do not match its boundary surface");
    }
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> regions;
    positions.reserve(model.surface.triangles.size() * 9);
    normals.reserve(model.surface.triangles.size() * 9);
    regions.reserve(model.surface.triangles.size() * 3);
    for (std::size_t triangle_index = 0; triangle_index < model.surface.triangles.size();
         ++triangle_index) {
        const auto& triangle = model.surface.triangles[triangle_index];
        const auto& a = model.surface.vertices.at(triangle[0]);
        const auto& b = model.surface.vertices.at(triangle[1]);
        const auto& c = model.surface.vertices.at(triangle[2]);
        Eigen::Vector3d normal = (b - a).cross(c - a);
        const double length = normal.norm();
        if (!(length > 0.0)) {
            throw std::runtime_error("model boundary contains a degenerate triangle");
        }
        normal /= length;
        const int region = model.triangle_region[triangle_index];
        for (const auto* point : {&a, &b, &c}) {
            append_point(positions, *point);
            append_point(normals, normal);
            regions.push_back(static_cast<float>(region));
        }
    }
    nlohmann::json value;
    value["n_verts"] = positions.size() / 3;
    value["positions_b64"] = base64_f32(positions);
    value["normals_b64"] = base64_f32(normals);
    value["region_b64"] = base64_f32(regions);
    return value;
}

EncodedMeshSurface mesh_surface_json(const fea::NodalMesh& mesh, std::size_t vertex_budget) {
    const auto faces = boundary_faces(mesh);
    std::map<std::size_t, std::vector<const BoundaryFace*>> by_owner;
    for (const auto& face : faces) {
        by_owner[face.owner].push_back(&face);
    }
    auto vertex_count_for_stride = [&](std::size_t stride) {
        std::size_t count = 0;
        std::size_t ordinal = 0;
        for (const auto& [owner, owner_faces] : by_owner) {
            (void)owner;
            if (ordinal++ % stride != 0) {
                continue;
            }
            for (const BoundaryFace* face : owner_faces) {
                count += 3 * (face->loop.size() - 2);
            }
        }
        return count;
    };
    std::size_t stride = 1;
    while (vertex_count_for_stride(stride) > vertex_budget && stride < by_owner.size()) {
        ++stride;
    }

    std::vector<float> positions;
    std::vector<float> centroids;
    std::vector<float> indices;
    std::vector<float> colors;
    std::vector<float> edges;
    positions.reserve(std::min(vertex_count_for_stride(stride), vertex_budget) * 3);
    std::size_t emitted = 0;
    std::size_t ordinal = 0;
    for (const auto& [owner, owner_faces] : by_owner) {
        if (ordinal++ % stride != 0) {
            continue;
        }
        std::size_t owner_vertices = 0;
        for (const BoundaryFace* face : owner_faces) {
            owner_vertices += 3 * (face->loop.size() - 2);
        }
        const std::size_t current_vertices = positions.size() / 3;
        if (owner_vertices > vertex_budget - std::min(vertex_budget, current_vertices)) {
            continue;
        }
        ++emitted;
        const Eigen::Vector3d centroid = element_centroid(mesh, owner);
        const auto color = element_color(mesh.elements[owner].type);
        const float normalized =
            mesh.elements.size() <= 1
                ? 0.0F
                : static_cast<float>(owner) / static_cast<float>(mesh.elements.size() - 1);
        std::set<std::pair<std::uint32_t, std::uint32_t>> owner_edges;
        for (const BoundaryFace* face : owner_faces) {
            FaceLoop loop = face->loop;
            Eigen::Vector3d normal = Eigen::Vector3d::Zero();
            for (std::size_t index = 0; index < loop.size(); ++index) {
                normal +=
                    mesh.nodes[loop[index]].cross(mesh.nodes[loop[(index + 1) % loop.size()]]);
            }
            Eigen::Vector3d center = Eigen::Vector3d::Zero();
            for (const std::uint32_t node : loop) {
                center += mesh.nodes[node];
            }
            center /= static_cast<double>(loop.size());
            if (normal.dot(center - centroid) < 0.0) {
                std::reverse(loop.begin(), loop.end());
            }
            for (std::size_t index = 1; index + 1 < loop.size(); ++index) {
                for (const std::uint32_t node : {loop[0], loop[index], loop[index + 1]}) {
                    append_point(positions, mesh.nodes[node]);
                    append_point(centroids, centroid);
                    indices.push_back(normalized);
                    colors.insert(colors.end(), color.begin(), color.end());
                }
            }
            for (std::size_t index = 0; index < loop.size(); ++index) {
                const std::uint32_t a = loop[index];
                const std::uint32_t b = loop[(index + 1) % loop.size()];
                owner_edges.emplace(std::min(a, b), std::max(a, b));
            }
        }
        for (const auto& [a, b] : owner_edges) {
            append_point(edges, mesh.nodes[a]);
            append_point(edges, mesh.nodes[b]);
        }
    }
    EncodedMeshSurface result;
    result.surface = arrays_json(std::move(positions), std::move(centroids),
                                 std::move(indices), std::move(colors), std::move(edges));
    result.emitted_elements = emitted;
    return result;
}

nlohmann::json result_surface_json(const pipeline::SolveResult& result) {
    const auto tessellation = fea::tessellate_boundary_surface(result.volume_mesh, 8);
    if (result.von_mises.size() != result.volume_mesh.nodes.size() ||
        result.u_magnitude.size() != result.volume_mesh.nodes.size() ||
        result.nodal_eta.size() != result.volume_mesh.nodes.size() ||
        result.displacement.size() !=
            3 * static_cast<Eigen::Index>(result.volume_mesh.nodes.size())) {
        throw std::runtime_error("solve result fields do not match the solved mesh");
    }
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> displacement;
    std::vector<float> von_mises;
    std::vector<float> u_magnitude;
    std::vector<float> eta;
    std::vector<float> edges;
    positions.reserve(tessellation.triangles.size() * 9);
    normals.reserve(tessellation.triangles.size() * 9);
    for (const auto& triangle : tessellation.triangles) {
        const auto& a = tessellation.samples.at(triangle[0]);
        const auto& b = tessellation.samples.at(triangle[1]);
        const auto& c = tessellation.samples.at(triangle[2]);
        Eigen::Vector3d normal = (b.position - a.position).cross(c.position - a.position);
        const double length = normal.norm();
        if (!(length > 0.0)) {
            throw std::runtime_error("result boundary contains a degenerate display triangle");
        }
        normal /= length;
        for (const fea::SurfaceSample* sample : {&a, &b, &c}) {
            append_point(positions, sample->position);
            append_point(normals, normal);
            append_point(displacement, interpolate_displacement(*sample, result.displacement));
            von_mises.push_back(
                static_cast<float>(interpolate_scalar(*sample, result.von_mises)));
            u_magnitude.push_back(
                static_cast<float>(interpolate_scalar(*sample, result.u_magnitude)));
            eta.push_back(static_cast<float>(interpolate_scalar(*sample, result.nodal_eta)));
        }
        for (const std::pair<const fea::SurfaceSample*, const fea::SurfaceSample*> edge :
             {std::pair{&a, &b}, std::pair{&b, &c}, std::pair{&c, &a}}) {
            append_point(edges, edge.first->position);
            append_point(edges, edge.second->position);
        }
    }
    nlohmann::json value;
    value["n_verts"] = positions.size() / 3;
    value["positions_b64"] = base64_f32(positions);
    value["normals_b64"] = base64_f32(normals);
    value["disp_b64"] = base64_f32(displacement);
    value["von_mises_b64"] = base64_f32(von_mises);
    value["u_mag_b64"] = base64_f32(u_magnitude);
    value["eta_b64"] = base64_f32(eta);
    value["n_edge_verts"] = edges.size() / 3;
    value["edges_b64"] = base64_f32(edges);
    return value;
}

} // namespace polymesh::webd
