// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Shared FEA scalar colormaps. Single source of truth for the blue→cyan→green→
// yellow→red ramp used by the 3D viewport (per-vertex result colors) and by the
// results colorbar in the UI panel, plus the diverging ramp the cinema surface
// uses for signed quantities. Header-only so both translation units get the
// exact same math — a drifting duplicate would make the legend lie.

#include <algorithm>
#include <array>
#include <cstddef>

namespace polymesh::gui {

/// Maps a scalar in [0,1] to a blue->cyan->green->yellow->red FEA colormap.
inline std::array<float, 3> fea_colormap(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float r = std::clamp(std::min(4.0f * t - 2.0f, 4.0f - 4.0f * t) + 1.0f, 0.0f, 1.0f);
    const float g = std::clamp(std::min(4.0f * t, 3.4f - 3.0f * t), 0.0f, 1.0f);
    const float b = std::clamp(2.0f - 4.0f * t, 0.0f, 1.0f);
    return {t > 0.75f ? 1.0f : r * 0.9f, g * 0.85f, b};
}

/// Maps a signed scalar in [-1,1] to the diverging blue -> near-white -> red
/// ramp, which is the same convention the figure scripts use for signed fields
/// (`figstyle.field_cmap("signed")`, i.e. matplotlib `RdBu_r`) — so a network
/// activation drawn in the GUI reads the same way as a residual plotted in a
/// paper figure. The control colours below ARE that map's control colours: the
/// eleven ColorBrewer RdBu class colours in reverse order, linearly
/// interpolated, which is exactly how matplotlib builds RdBu from them.
/// Sampled at 257 points against matplotlib's own RdBu_r the two agree to
/// 0.0115 in unit RGB (3/255) — that residual is matplotlib's 256-bin
/// quantisation, not a different ramp.
///
/// Zero lands on the neutral centre, so callers must map a SYMMETRIC range
/// onto [-1,1]; an asymmetric one puts white somewhere that means nothing.
/// This is the same requirement figstyle.py documents for its signed fields.
inline std::array<float, 3> signed_colormap(float t) {
    // ColorBrewer RdBu, 11 classes, reversed so negative = blue. Written as the
    // original 8-bit values over 255 to keep them checkable against the source.
    static constexpr std::array<std::array<float, 3>, 11> kRdBuReversed = {{
        {5.0f / 255, 48.0f / 255, 97.0f / 255},    {33.0f / 255, 102.0f / 255, 172.0f / 255},
        {67.0f / 255, 147.0f / 255, 195.0f / 255}, {146.0f / 255, 197.0f / 255, 222.0f / 255},
        {209.0f / 255, 229.0f / 255, 240.0f / 255},
        {247.0f / 255, 247.0f / 255, 247.0f / 255},
        {253.0f / 255, 219.0f / 255, 199.0f / 255}, {244.0f / 255, 165.0f / 255, 130.0f / 255},
        {214.0f / 255, 96.0f / 255, 77.0f / 255},  {178.0f / 255, 24.0f / 255, 43.0f / 255},
        {103.0f / 255, 0.0f / 255, 31.0f / 255},
    }};
    constexpr int last = static_cast<int>(kRdBuReversed.size()) - 1;
    t = std::clamp(t, -1.0f, 1.0f);
    const float u = 0.5f * (t + 1.0f) * static_cast<float>(last);
    // u == last exactly at t == 1; clamping `lo` keeps `hi` in range there.
    const int lo = std::min(static_cast<int>(u), last);
    const int hi = std::min(lo + 1, last);
    const float f = u - static_cast<float>(lo);
    const auto& a = kRdBuReversed[static_cast<std::size_t>(lo)];
    const auto& b = kRdBuReversed[static_cast<std::size_t>(hi)];
    return {a[0] + f * (b[0] - a[0]), a[1] + f * (b[1] - a[1]), a[2] + f * (b[2] - a[2])};
}

} // namespace polymesh::gui
