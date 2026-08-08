// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Shared FEA scalar colormap. Single source of truth for the blue→cyan→green→
// yellow→red ramp used by the 3D viewport (per-vertex result colors) and by the
// results colorbar in the UI panel. Header-only so both translation units get
// the exact same math — a drifting duplicate would make the legend lie.

#include <algorithm>
#include <array>

namespace polymesh::gui {

/// Maps a scalar in [0,1] to a blue->cyan->green->yellow->red FEA colormap.
inline std::array<float, 3> fea_colormap(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float r = std::clamp(std::min(4.0f * t - 2.0f, 4.0f - 4.0f * t) + 1.0f, 0.0f, 1.0f);
    const float g = std::clamp(std::min(4.0f * t, 3.4f - 3.0f * t), 0.0f, 1.0f);
    const float b = std::clamp(2.0f - 4.0f * t, 0.0f, 1.0f);
    return {t > 0.75f ? 1.0f : r * 0.9f, g * 0.85f, b};
}

} // namespace polymesh::gui
