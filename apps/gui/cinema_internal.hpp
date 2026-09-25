// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Helpers shared by the cinema_*.cpp translation units. Private to
// polymesh-gui; the module's interface is cinema.hpp.

#include "cinema.hpp"

#include "imgui.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace polymesh::gui::detail {

/// printf into a std::string. Captions exist as strings before drawing so a row
/// can be sized to fit; the printf formats are kept so no displayed number
/// changes representation.
#if defined(__GNUC__)
[[gnu::format(printf, 1, 2)]]
#endif
std::string fmt(const char* format, ...);

/// `n` with thousands separators ("11,692").
std::string grouped(std::size_t n);

/// Plain-English names for the `pipeline::kMeshStageNames` ids. The ids stay in
/// the manifest; unknown ids pass through verbatim.
std::string_view stage_name(std::string_view stage);

/// Plain-English names for the `pipeline::mesher_name` vocabulary (the model's
/// training and CLI vocabulary is never rewritten); unknown names pass through.
std::string_view mesher_plain(std::string_view mesher);

/// Cubic smoothstep on [0,1]. Opacity, shrink and sweep-front easing only --
/// never applied to a displayed number.
double smoothstep(double x);

/// `c` with its alpha multiplied by `alpha`, packed for ImDrawList.
ImU32 faded(ImVec4 c, float alpha);

/// Number of construction stages of the initial fill (pass 0), the fill the
/// build act shows. Later passes' remesh stages are not replayed; the closing
/// act shows the meshes those passes solved.
std::size_t initial_fill_stage_count(const std::vector<pipeline::MeshStage>& stages);

/// Element mix of one measured mesh ("N tet4 · M hex8"), or "no cells measured".
std::string mesh_mix_text(const CinemaMeshInsight& insight);

} // namespace polymesh::gui::detail
