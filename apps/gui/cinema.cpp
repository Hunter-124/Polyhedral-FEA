// SPDX-License-Identifier: BSD-3-Clause
#include "cinema.hpp"

#include "colormap.hpp"
#include "geom/signal_fft.hpp"
#include "theme.hpp"

#include "fea/cell_quality.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"

#include "imgui.h"
#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace polymesh::gui {
namespace {

/// Act spans as fractions of the 60 s public take.
///
/// The opening starts directly on the analysed CAD body. The cell microscope
/// receives a full ten seconds: enough to build a connected linear patch,
/// elevate the same topology to quadratic order, and hold both states for
/// comparison. The solve still has 21 seconds, and its recorded phases scale
/// proportionally to fit that window.
///
///   exact CAD / edge analysis    10.8 s
///   advisor                      7.8 s
///   emitted mesh                10.2 s
///   tet4 → tet10 microscope     10.2 s
///   solve / recovery            21.0 s
constexpr std::array<double, kCinemaActCount> kActFraction = {0.18, 0.13, 0.17, 0.17, 0.35};
double beat_seconds(SolvePhase phase) {
    switch (phase) {
    case SolvePhase::kStressSweep:
        return 2.6;
    case SolvePhase::kStressHold:
        return 3.2;
    case SolvePhase::kGradientSweep:
        return 2.6;
    case SolvePhase::kGradientHold:
        return 3.0;
    case SolvePhase::kError:
        return 2.4;
    case SolvePhase::kErrorHold:
        return 2.8;
    case SolvePhase::kRefine:
        return 3.2;
    case SolvePhase::kRefineHold:
        return 3.4;
    case SolvePhase::kLoadRamp:
        return 4.4;
    case SolvePhase::kHold:
        return 5.4;
    case SolvePhase::kNone:
        break;
    }
    return 0.0;
}

/// Per-element reveal geometry, retuned for the denser complex-CAD take.
///
/// The old full-opacity 1.5 px edge pass was measured on a 568-cell fixture.
/// At 11,692 cells it already put up to half the subject into near-black
/// outline; the complex showcase carries tens of thousands of cells. Keep the
/// centroid shrink that lets a new cell read as a separate object, but make the
/// edge pass a restrained annotation over shaded faces rather than the image.
constexpr double kRevealShrink = 0.14;
constexpr double kRevealShrinkFraction = 0.24;
constexpr float kMeshEdgeAlpha = 0.34f;
constexpr float kMeshEdgeWidth = 1.0f;

/// Width of the "just arrived" cell highlight trailing the reveal front, as a
/// fraction of the mesh's element count. At the showcase's 40k cells this is
/// roughly 1.8k cells: enough that the front reads as cells being drawn one
/// after another, small enough that the settled mesh keeps its own element
/// colour instead of the part turning into a highlight.
constexpr float kArrivalBand = 0.045f;

/// Width of the field sweep's leading band, as a fraction of the part's extent
/// along the sweep axis. Wide enough that the highlight reads as a moving front
/// rather than a hard edge, narrow enough that it is a front and not a fade.
constexpr float kSweepFeather = 0.10f;

/// The completed target-spacing field remains as a restrained annotation while
/// the advisor starts scoring, then disappears only as the mesher replaces
/// targets with actual cells.
constexpr float kSizingCarryAlpha = 0.22f;
/// Fraction of a refinement beat spent dissolving the measured ZZ field into
/// the exact old→new topology transition it requested.
constexpr double kFieldToMeshHandoff = 0.32;
/// Fraction of a stress reveal over which the authoritative cell rendering
/// remains on top, then yields to the solved field arriving beneath it.
constexpr double kMeshToFieldHandoff = 0.42;

/// Floor on λ when it is used as a DIVISOR for the colour scale. λ itself is
/// never floored — the number on screen and the displacement are the real λ,
/// including exactly 0 — but s_max / λ has to stay finite, and at λ = 1e-3 every
/// drawn colour is already within one part in a thousand of the bottom of the
/// colormap, which is the correct picture of a part carrying no load.
constexpr double kMinLoadFactor = 1.0e-3;

/// Connections drawn per frame. The deployed graph has 15,936; drawing all of
/// them fills the lane stack with a grey haze, so only the strongest subset is
/// drawn and the count is disclosed from this same constant.
///
/// 180 rather than the 280 this used to draw: the panel is now set at nearly
/// twice the type size, so the same haze costs twice the readable area, and the
/// weakest hundred of the old set were hairlines carrying |w·a| under a tenth of
/// the strongest.
constexpr std::size_t kDrawnConnections = 180;

/// Widest an activation node is allowed to get.
constexpr float kNodeRadiusMax = 8.0f;

/// printf into a std::string.
///
/// The captions have to exist as strings BEFORE anything is drawn, so a row can
/// be set at whatever size makes it fit. They keep the printf format strings
/// this surface already used: rewriting a hundred conversions into another
/// formatting library would be a hundred chances to change a displayed number,
/// which is the one thing this file may not do.
#if defined(__GNUC__)
[[gnu::format(printf, 1, 2)]]
#endif
std::string fmt(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::va_list measure;
    va_copy(measure, args);
    const int n = std::vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    std::string out;
    if (n > 0) {
        out.resize(static_cast<std::size_t>(n));
        std::vsnprintf(out.data(), static_cast<std::size_t>(n) + 1, format, args);
    }
    va_end(args);
    return out;
}

/// Thousands separators. "11,692 cells" is read at a glance and "11692 cells"
/// is counted, and this film is aimed at readers who are doing the former.
std::string grouped(std::size_t n) {
    std::string digits = std::format("{}", n);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t lead = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (i - lead) % 3 == 0 && i >= lead) {
            out.push_back(',');
        }
        out.push_back(digits[i]);
    }
    return out;
}

/// Plain-English names for the graph's own head units.
///
/// The names on the left of this table are the deployed model's own tensor
/// labels, read out of `activation_layout.json`; the artifact is NOT rewritten,
/// because it is the exporter's record of what the graph emits and a film is not
/// a reason to edit a model artifact. The names on the right are what a reader
/// who has never seen this codebase can understand. The mapping is stated on
/// screen ("head names in plain language") and the artifact's own spelling for
/// every one of them is in docs/assets/cinema/NOTES.md.
///
/// An unmapped label falls through VERBATIM rather than being prettified by a
/// rule: a silent underscore-to-space transform would have turned a head this
/// table has not been taught about into a confident-looking label nobody chose.
struct HeadName {
    std::string_view tensor;
    const char* plain;
};
constexpr std::array<HeadName, 20> kHeadNames{{
    {"rel_err", "predicted error"},
    {"rel_err_rel", "error vs this part's median"},
    {"geo_chamfer", "mesh-to-CAD distance"},
    {"geo_p99", "mesh-to-CAD worst 1%"},
    {"dof", "unknowns"},
    {"mesh_ms", "meshing time"},
    {"solve_ms", "solve time"},
    {"solve_flops", "portable solve work"},
    {"solve_bytes", "portable data traffic"},
    {"mesh_work", "host-normalized meshing work"},
    {"failure_logit", "failure risk"},
    {"policy_h_rel", "cell size"},
    {"policy_adapt_passes", "refinement passes"},
    {"policy_eta_target", "error target"},
    {"policy_order_logit_1", "order 1 (linear)"},
    {"policy_order_logit_2", "order 2 (quadratic)"},
    {"policy_mesher_logit_graded_tet", "mesher: graded tets"},
    {"policy_mesher_logit_hex", "mesher: hex"},
    {"policy_mesher_logit_hybrid_vem", "mesher: hybrid VEM"},
    {"policy_mesher_logit_hybrid_zoo", "mesher: hybrid, hex + pyramids"},
}};

[[maybe_unused]] std::string_view head_name(std::string_view tensor) {
    for (const auto& entry : kHeadNames) {
        if (entry.tensor == tensor) {
            return entry.plain;
        }
    }
    return tensor;
}

/// Plain-English names for the mesher vocabulary `pipeline::mesher_name`
/// returns. The vocabulary strings themselves are what the model was trained on
/// and what the CLI accepts, so they are never rewritten anywhere but here.
std::string_view mesher_plain(std::string_view mesher) {
    if (mesher == "graded_tet") {
        return "graded tets";
    }
    if (mesher == "hybrid_zoo") {
        return "hybrid: hex bulk, pyramid skin";
    }
    if (mesher == "hybrid_vem") {
        return "hybrid with polyhedral transitions";
    }
    if (mesher == "hex" || mesher == "hex_fill") {
        return "hexes";
    }
    if (mesher == "hex_vem") {
        return "hexes with polyhedral transitions";
    }
    if (mesher == "tet") {
        return "tets";
    }
    return mesher;
}

std::string_view mesh_stage_plain(std::string_view stage) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kNames{{
        {"lattice", "seed lattice"},
        {"expand", "cell expansion"},
        {"snap", "CAD projection"},
        {"peel", "quality repair"},
        {"reproject", "boundary recovery"},
        {"smooth", "surface smoothing"},
        {"resnap", "CAD resnap"},
        {"pin", "feature pinning"},
        {"fill", "solver cells"},
        {"ship", "validated mesh"},
    }};
    for (const auto& [id, plain] : kNames) {
        if (stage == id) {
            return plain;
        }
    }
    return stage;
}

/// Cubic smoothstep on [0,1]. Used for opacity, the shrink collapse and the
/// sweep front only -- never for a displayed number.
double smoothstep(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

ImU32 rgba(const std::array<float, 3>& rgb, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(rgb[0], rgb[1], rgb[2], alpha));
}

ImU32 faded(ImVec4 c, float alpha) {
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

/// Axis-aligned box of one tessellation region, in model coordinates (metres).
///
/// `pipeline::extract_case_features` takes fix/load context as
/// `RefineRegion` boxes, which is what the CLI's `--fix-box` / `--load-box`
/// supply by hand. The studio selects boundary conditions by face id instead,
/// so the box a GUI face contributes is the box of that face's own triangles.
/// Returns nullopt for a region with no triangles, so an empty selection
/// contributes nothing rather than a degenerate box at the origin.
std::optional<std::pair<Eigen::Vector3d, Eigen::Vector3d>>
region_box(const pipeline::Model& model, int region) {
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();
    bool any = false;
    const auto& tris = model.surface.triangles;
    for (std::size_t ti = 0; ti < tris.size(); ++ti) {

[Showing lines 1-300 of 4094. Use :301 to continue]