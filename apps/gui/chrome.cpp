// SPDX-License-Identifier: BSD-3-Clause

// Window chrome bootstrap: UI/film fonts, the startup window size override and
// the procedural window icon.

#include "app_state.hpp"
#include "cinema.hpp"
#include "theme.hpp"

#include "imgui.h"

// On Windows, glad owns GL symbols — keep GLFW from including system gl.h.
#if defined(_WIN32)
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <glad/glad.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace polymesh::gui {

namespace {

/// Fills the maths gaps (U+2190..U+22FF only) in the film face from the first
/// maths face found. ImGui keeps the FIRST glyph added per codepoint, so the
/// merge never overrides a glyph the primary face has; the choice is printed.
void merge_maths_glyphs(ImGuiIO& io, float size) {
    static const ImWchar kMathsRanges[] = {
        0x2190, 0x21FF, // arrows (→, ⇒)
        0x2200, 0x22FF, // maths operators (∇, √, ∫, ‖-adjacent, −, ≥)
        0,
    };
    static constexpr const char* kMathsFaces[] = {
        "/usr/share/fonts/google-noto/NotoSansMath-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/gdouros-symbola/Symbola.ttf",
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/cambria.ttc",
        "/System/Library/Fonts/Supplemental/Symbola.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
    };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    for (const char* path : kMathsFaces) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path{path}, ec)) {
            continue;
        }
        if (io.Fonts->AddFontFromFileTTF(path, size, &cfg, kMathsRanges) != nullptr) {
            std::printf("cinema: maths glyphs merged from %s\n", path);
            std::fflush(stdout);
            return;
        }
    }
    std::printf("cinema: no maths fallback face found; the equation board draws ∇ and the "
                "rest of U+2190..U+22FF from the UI face alone\n");
    std::fflush(stdout);
}

/// Signed distance to a regular hexagon of circumradius `r` centered on the
/// origin (negative inside). Icon drawing only — no scene math.
float hexagon_sdf(float px, float py, float r) {
    constexpr float kx = -0.8660254f;
    constexpr float ky = 0.5f;
    constexpr float kz = 0.5773503f;
    px = std::fabs(px);
    py = std::fabs(py);
    const float fold = 2.0f * std::min(kx * px + ky * py, 0.0f);
    px -= fold * kx;
    py -= fold * ky;
    px -= std::clamp(px, -kz * r, kz * r);
    py -= r;
    return std::sqrt(px * px + py * py) * (py < 0.0f ? -1.0f : 1.0f);
}

} // namespace

/// $POLYMESH_GUI_FONT first, then the usual per-platform locations. Existence is
/// checked first because AddFontFromFileTTF asserts on a missing file in debug
/// builds. The ranges cover every symbol the UI and the equation board draw;
/// sub-/superscripts are composed from ordinary digits instead, so they are
/// absent on purpose. The film face merges a maths fallback (merge_maths_glyphs).
bool load_ui_font(ImFont** cinema_out) {
    ImGuiIO& io = ImGui::GetIO();
    // Static: ImGui keeps the pointer until the atlas is built.
    static const ImWchar kRanges[] = {
        0x0020, 0x00FF, // Latin + Latin-1 supplement (°, µ, ±, ½, ²)
        0x0370, 0x03FF, // Greek (σ, ν, Ω, θ, η, λ, ε)
        0x2010, 0x203A, // dashes, quotes, ·, —, ‖
        0x2190, 0x21FF, // arrows (→, ⇒)
        0x2200, 0x22FF, // maths operators (∇, √, ∫, −, ≈, ≤, ≥, ×, ∞)
        0x2300, 0x2300, // ⌀ diameter sign
        0,
    };
    auto try_load = [&io, cinema_out](const char* path) {
        std::error_code ec;
        if (path == nullptr || path[0] == '\0' ||
            !std::filesystem::is_regular_file(std::filesystem::path{path}, ec)) {
            return false;
        }
        if (io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, kRanges) == nullptr) {
            return false;
        }
        // A film face that fails to load is not a reason to lose the UI face
        // that just did: the film degrades to soft text, the studio does not
        // degrade at all.
        if (cinema_out != nullptr) {
            *cinema_out =
                io.Fonts->AddFontFromFileTTF(path, kCinemaAtlasSize, nullptr, kRanges);
            if (*cinema_out != nullptr) {
                merge_maths_glyphs(io, kCinemaAtlasSize);
            }
        }
        return true;
    };
    if (try_load(std::getenv("POLYMESH_GUI_FONT"))) {
        return true;
    }
    static constexpr const char* kFallbacks[] = {
        // Linux
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        // Windows
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        // macOS
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
    };
    for (const char* path : kFallbacks) {
        if (try_load(path)) {
            return true;
        }
    }
    return false;
}

bool parse_window_size(const char* text, int& width, int& height) {
    if (text == nullptr) {
        return false;
    }
    const char* sep = std::strchr(text, 'x');
    if (sep == nullptr || sep == text || sep[1] == '\0') {
        return false;
    }
    const std::string w_text(text, sep);
    int w = 0;
    int h = 0;
    if (!parse_auto_int(w_text, w) || !parse_auto_int(std::string(sep + 1), h)) {
        return false;
    }
    // Lower bound is the window's own minimum size limit; upper bound keeps a
    // fat-fingered value from asking GL for a framebuffer no driver will make.
    if (w < 960 || h < 640 || w > 16384 || h > 16384) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

/// Runtime-generated 64x64 window icon: a hexagonal cell stroked in the Studio
/// accent over a graphite body, fully transparent outside the cell. Procedural
/// so the app ships no image assets.
void set_window_icon(GLFWwindow* window) {
    constexpr int kSize = 64;
    constexpr float kHalf = 32.0f;
    constexpr float kOuter = 24.0f; // apothem; half-width is 24/cos30 = 27.7 px
    constexpr float kInner = 12.0f; // inner ring reads as a graded cell
    unsigned char pixels[kSize * kSize * 4];
    // Studio tokens: graphite cell body, dim accent inner ring, accent stroke.
    const Palette studio = make_studio_palette();
    const ImVec4& body_rgb = studio.panel_bg;
    const ImVec4& inner_rgb = studio.accent_dim;
    const ImVec4& ring_rgb = studio.accent;
    // Analytic one-pixel coverage from a signed distance (1 inside, 0 outside).
    auto coverage = [](float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); };
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float px = static_cast<float>(x) + 0.5f - kHalf;
            const float py = static_cast<float>(y) + 0.5f - kHalf;
            const float d_out = hexagon_sdf(px, py, kOuter);
            const float d_in = hexagon_sdf(px, py, kInner);
            const float body = coverage(d_out);
            const float ring = coverage(std::fabs(d_out) - 1.3f);          // accent stroke
            const float ring_in = coverage(std::fabs(d_in) - 0.9f) * body; // dim stroke
            float r = body_rgb.x, g = body_rgb.y, b = body_rgb.z;
            float a = body * 0.92f;
            r = r * (1.0f - ring_in) + inner_rgb.x * ring_in;
            g = g * (1.0f - ring_in) + inner_rgb.y * ring_in;
            b = b * (1.0f - ring_in) + inner_rgb.z * ring_in;
            a = std::max(a, ring_in);
            r = r * (1.0f - ring) + ring_rgb.x * ring;
            g = g * (1.0f - ring) + ring_rgb.y * ring;
            b = b * (1.0f - ring) + ring_rgb.z * ring;
            a = std::max(a, ring);
            const std::size_t i =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(kSize) +
                 static_cast<std::size_t>(x)) *
                4u;
            pixels[i + 0] = static_cast<unsigned char>(std::lround(r * 255.0f));
            pixels[i + 1] = static_cast<unsigned char>(std::lround(g * 255.0f));
            pixels[i + 2] = static_cast<unsigned char>(std::lround(b * 255.0f));
            pixels[i + 3] =
                static_cast<unsigned char>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
    }
    const GLFWimage image{kSize, kSize, pixels};
    glfwSetWindowIcon(window, 1, &image);
}

} // namespace polymesh::gui
