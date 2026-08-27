// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "pipeline/scene.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace polymesh::webd {

inline constexpr std::size_t kStageVertexBudget = 600000;

[[nodiscard]] std::string base64_bytes(std::span<const std::byte> bytes);
[[nodiscard]] std::string base64_f32(std::span<const float> values);
[[nodiscard]] nlohmann::json part_surface_json(const pipeline::Model& model);

struct EncodedMeshSurface {
    nlohmann::json surface;
    std::size_t emitted_elements = 0;
};

[[nodiscard]] EncodedMeshSurface
mesh_surface_json(const fea::NodalMesh& mesh, std::size_t vertex_budget = kStageVertexBudget);
[[nodiscard]] nlohmann::json result_surface_json(const pipeline::SolveResult& result);

} // namespace polymesh::webd
