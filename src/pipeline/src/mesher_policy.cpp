// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

namespace polymesh::pipeline {

std::string_view mesher_name(VolumeMesher mesher) {
    switch (mesher) {
    case VolumeMesher::kTetFill:
        return "tet";
    case VolumeMesher::kHexFill:
        return "hex";
    case VolumeMesher::kHexVem:
        return "hex_vem";
    case VolumeMesher::kGradedTet:
        return "graded_tet";
    case VolumeMesher::kHexPyramid:
        return "hexpyr";
    case VolumeMesher::kPrismSweep:
        return "prism";
    case VolumeMesher::kHybrid:
        return "hybrid_zoo";
    case VolumeMesher::kOctahedral:
        return "octa";
    case VolumeMesher::kHybridVem:
        return "hybrid_vem";
    case VolumeMesher::kVaryhedron:
        return "varyhedron";
    case VolumeMesher::kCvtPoly:
        return "cvt_poly";
    }
    // No `default:` above, so a new enumerator fails the -Wswitch build instead
    // of silently sharing a label with another mesher.
    return "unknown"; // only reachable from an out-of-range int cast
}

std::optional<VolumeMesher> mesher_from_name(std::string_view name) {
    // Every accepted alias, including each enumerator's canonical name above,
    // so `mesher_from_name(mesher_name(m)) == m` holds for all eleven.
    // `hybrid_zoo`, `graded_tet`, `hex_vem` and `hybrid_vem` are the advisor
    // spellings; `hybrid`, `graded`, `hexvem` and `hybridvem` the CLI ones.
    static constexpr std::array<std::pair<std::string_view, VolumeMesher>, 26> kAliases{{
        {"tet", VolumeMesher::kTetFill},          {"tet_fill", VolumeMesher::kTetFill},
        {"hex", VolumeMesher::kHexFill},          {"hex_vem", VolumeMesher::kHexVem},
        {"hexvem", VolumeMesher::kHexVem},        {"vem", VolumeMesher::kHexVem},
        {"graded_tet", VolumeMesher::kGradedTet}, {"graded", VolumeMesher::kGradedTet},
        {"hexpyr", VolumeMesher::kHexPyramid},    {"transition", VolumeMesher::kHexPyramid},
        {"prism", VolumeMesher::kPrismSweep},     {"sweep", VolumeMesher::kPrismSweep},
        {"hybrid_zoo", VolumeMesher::kHybrid},    {"hybrid", VolumeMesher::kHybrid},
        {"zoo", VolumeMesher::kHybrid},           {"mixed", VolumeMesher::kHybrid},
        {"octa", VolumeMesher::kOctahedral},      {"octahedral", VolumeMesher::kOctahedral},
        {"hybrid_vem", VolumeMesher::kHybridVem}, {"hybridvem", VolumeMesher::kHybridVem},
        {"hybrid-vem", VolumeMesher::kHybridVem}, {"varyhedron", VolumeMesher::kVaryhedron},
        {"vary", VolumeMesher::kVaryhedron},      {"cvt_poly", VolumeMesher::kCvtPoly},
        {"cvt", VolumeMesher::kCvtPoly},          {"restricted_cvt", VolumeMesher::kCvtPoly},
    }};
    for (const auto& [alias, mesher] : kAliases) {
        if (alias == name) {
            return mesher;
        }
    }
    return std::nullopt;
}

ElementTendencyPlan resolve_element_tendency(VolumeMesher base, double tendency,
                                             int skin_layers) {
    ElementTendencyPlan plan;
    plan.tendency = std::clamp(tendency, -1.0, 1.0);
    plan.skin_layers = std::max(1, skin_layers);
    plan.mesher = base;
    plan.native_poly_transitions = (base == VolumeMesher::kHybridVem);
    plan.remapped = false;

    const auto label_for = [](VolumeMesher m) -> const char* {
        switch (m) {
        case VolumeMesher::kHexFill:
            return "hex";
        case VolumeMesher::kHexVem:
            return "hex-vem";
        case VolumeMesher::kHybrid:
            return "hybrid-fan";
        case VolumeMesher::kHybridVem:
            return "hybrid-vem";
        case VolumeMesher::kGradedTet:
            return "graded-tet";
        case VolumeMesher::kVaryhedron:
            return "varyhedron";
        case VolumeMesher::kCvtPoly:
            return "cvt_poly";
        case VolumeMesher::kTetFill:
            return "tet";
        case VolumeMesher::kHexPyramid:
            return "hex-pyramid";
        case VolumeMesher::kPrismSweep:
            return "prism";
        case VolumeMesher::kOctahedral:
            return "octahedral";
        }
        return "unknown";
    };
    plan.label = label_for(base);

    // Exact zero (campaign default / SimSetup default) preserves the base
    // mesher so kHybrid and kHybridVem product paths stay unchanged.
    if (std::abs(plan.tendency) < 1e-12) {
        return plan;
    }

    const auto is_hybrid_family = [](VolumeMesher m) {
        return m == VolumeMesher::kHybrid || m == VolumeMesher::kHybridVem;
    };
    const auto is_hex_family = [](VolumeMesher m) {
        return m == VolumeMesher::kHexFill || m == VolumeMesher::kHexVem;
    };
    const auto is_tet_family = [](VolumeMesher m) {
        return m == VolumeMesher::kTetFill || m == VolumeMesher::kGradedTet ||
               m == VolumeMesher::kVaryhedron;
    };

    // Shape dial for hybrid / hex / tet families. Prism / octa / hexpyr keep
    // their explicit base (no continuous remap yet).
    VolumeMesher effective = base;
    if (is_hybrid_family(base) || is_hex_family(base) || is_tet_family(base)) {
        if (plan.tendency <= -0.5) {
            effective = VolumeMesher::kHexFill;
        } else if (plan.tendency <= 0.25) {
            effective = VolumeMesher::kHybrid;
        } else if (plan.tendency <= 0.75) {
            effective = VolumeMesher::kHybridVem;
        } else {
            effective = VolumeMesher::kGradedTet;
        }
    }

    plan.mesher = effective;
    plan.native_poly_transitions = (effective == VolumeMesher::kHybridVem);
    plan.label = label_for(effective);
    plan.remapped = (effective != base);

    // Skin treatment: hex bias on hybrid → thinner free-surface skin (more
    // bulk hex); strong tet bias → one extra graded skin hop.
    if (effective == VolumeMesher::kHybrid && plan.tendency < 0.0) {
        const int thinned = std::max(1, skin_layers - 1);
        if (thinned != plan.skin_layers) {
            plan.skin_layers = thinned;
            plan.remapped = true;
        }
    } else if (effective == VolumeMesher::kGradedTet && plan.tendency > 0.75) {
        plan.skin_layers = skin_layers + 1;
        plan.remapped = true;
    }

    return plan;
}

} // namespace polymesh::pipeline
