// SPDX-License-Identifier: BSD-3-Clause

// Window chrome bootstrap: UI/film fonts, the startup window size override and
// the procedural window icon.

#include "app_state.hpp"
#include "cinema.hpp"
#include "theme.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

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

/// Fills the maths gaps in whatever face the film ended up with.
///
/// Merged rather than substituted: ImGui keeps the FIRST glyph added for a
/// codepoint, so this supplies U+2190..U+22FF only where the primary face had
/// nothing and never overrides a glyph it did have. Restricted to those two
/// blocks so the merge cannot quietly restyle Latin or Greek text either.
///
/// A box with none of these installed keeps whatever the primary face has, which
/// is the same outcome as before this existed. Either way the choice is PRINTED:
/// a glyph that silently fell back is the kind of defect that reaches a
/// committed asset and is then argued about, so the recorder's log says which
/// file supplied the maths.
/// Merges a symbol fallback face over whatever face was just added, filling the
/// codepoints the primary face does not carry. ImGui keeps the FIRST glyph
/// added for a codepoint, so this fills gaps and never overrides the primary.
///
/// MEASURED: the brand face is Rubik, which has no Greek block at all, so
/// Poisson's ratio rendered as "? 0.3" in the Material step the moment the
/// studio adopted the Chudware faces. Liberation Sans, the previous default,
/// happened to cover Greek and hid the need for this. The ranges below are
/// therefore every non-Latin block `kRanges` asks for — Greek included, not
/// just maths — and the merge is applied to the body, header and mono faces,
/// not only to the film's face.
void merge_symbol_glyphs(ImGuiIO& io, float size) {
    static const ImWchar kMathsRanges[] = {
        0x0370, 0x03FF, // Greek (σ, ν, Ω, θ, η, λ, ε) — absent from Rubik
        0x2010, 0x203A, // dashes, quotes, ·, —, ‖
        0x2190, 0x21FF, // arrows (→, ⇒)
        0x2200, 0x22FF, // maths operators (∇, √, ∫, −, ≈, ≤, ≥, ×, ∞)
        0x2300, 0x2300, // ⌀ diameter sign
        0,
    };
    // Every face that exists is merged, in order, up to `kMaxFallbackFaces`.
    // ImGui keeps the first glyph added for a codepoint, so the ordering is
    // load-bearing: NotoSansMath stays first and therefore remains the source
    // of the film's maths glyphs exactly as the cinema notes record, while a
    // broad face merged after it fills the Greek block that neither Rubik nor
    // a maths-only face carries. Merging one face and stopping is what left ν
    // as tofu.
    static constexpr const char* kFallbackFaces[] = {
        "/usr/share/fonts/google-noto/NotoSansMath-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/gdouros-symbola/Symbola.ttf",
        "C:/Windows/Fonts/seguisym.ttf",
        "C:/Windows/Fonts/cambria.ttc",
        "/System/Library/Fonts/Supplemental/Symbola.ttf",
        "/System/Library/Fonts/Apple Symbols.ttf",
    };
    constexpr int kMaxFallbackFaces = 3;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    int merged = 0;
    static bool logged = false;
    for (const char* path : kFallbackFaces) {
        if (merged >= kMaxFallbackFaces) {
            break;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path{path}, ec)) {
            continue;
        }
        if (io.Fonts->AddFontFromFileTTF(path, size, &cfg, kMathsRanges) != nullptr) {
            if (!logged) {
                std::printf("fonts: symbol fallback merged from %s\n", path);
            }
            merged += 1;
        }
    }
    if (merged == 0 && !logged) {
        std::printf("fonts: no symbol fallback face found; Greek, arrows and maths operators "
                    "draw from the UI face alone\n");
    }
    logged = true;
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

/// Loads Rubik at 16 px and the same face at `kCinemaAtlasSize`: an explicit
/// $POLYMESH_GUI_FONT first, then installed and source-tree Chudware assets,
/// then the existing platform fallbacks. JetBrains Mono is loaded separately
/// for telemetry. Missing assets are never fatal — ImGui's stock bitmap font
/// remains the final fallback. Paths are checked because AddFontFromFileTTF
/// asserts on a missing file in debug builds.
///
/// The second face is not a luxury. ImGui rasterises one pixel size per
/// `ImFont` and scales every other size from it, so the film's 40 px headline
/// drawn from the 16 px atlas is a 2.5x bitmap upscale: legible on a monitor,
/// mush once the README's GIF halves it again. `cinema_out` receives it, or
/// stays null when no TTF loaded at all, in which case the film draws from
/// whatever face is there.
///
/// The glyph range is explicit. ImGui's default range is Latin only, so the
/// labels that carry σ, ≥, ×, ⌀ or · rendered as "?" boxes — which then landed
/// in committed screenshots. Anything drawn in the UI has to be in the atlas,
/// and the equation board draws ∇, √, ‖, ∫, → and Ω out of the two blocks
/// below. Sub- and superscripts are NOT in the atlas on purpose: the board
/// composes them from scaled, raised runs of ordinary digits, because
/// Liberation Sans — the first Linux fallback here — has no U+2081 and an
/// equation full of tofu boxes is worse than no equation.
///
/// Which is also why the film's face MERGES a maths fallback over the UI face.
/// Measured on this box: Liberation Sans, Fedora's UI default and the first
/// entry below, has no U+2207 ∇ and no U+21D2 ⇒ — so the gradient equation drew
/// two tofu boxes, in a panel whose whole job is to be readable. Swapping the
/// studio's UI face for a maths font to fix a film is the wrong trade; merging
/// the missing block into the film's own face is not. ImGui keeps the FIRST
/// glyph added for a codepoint, so the merge fills gaps and never overrides a
/// glyph the UI face already has.
bool load_ui_font(float atlas_scale, ImFont** cinema_out, ImFont** mono_out) {
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
    ImFont* body = nullptr;
    auto try_body = [&](const std::filesystem::path& path) {
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
            return false;
        }
        body = io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f * atlas_scale,
                                            nullptr, kRanges);
        if (body == nullptr) {
            return false;
        }
        // The body face needs the fallback as much as the film's does: Rubik
        // carries no Greek, and Poisson's ratio is on the Material step.
        merge_symbol_glyphs(io, 16.0f * atlas_scale);
        if (cinema_out != nullptr) {
            *cinema_out = io.Fonts->AddFontFromFileTTF(
                path.string().c_str(), kCinemaAtlasSize * atlas_scale, nullptr, kRanges);
            if (*cinema_out != nullptr) {
                merge_symbol_glyphs(io, kCinemaAtlasSize * atlas_scale);
            }
        }
        return true;
    };

    std::error_code ec;
    const auto source_root =
        std::filesystem::path{__FILE__}.parent_path().parent_path().parent_path();
    const auto cwd = std::filesystem::current_path(ec);
    const auto installed_fonts = executable_dir / ".." / "share" / "polymesh" / "fonts";
    std::vector<std::filesystem::path> brand_roots{installed_fonts,
                                                   source_root / "assets/fonts"};
    if (!ec) {
        brand_roots.push_back(cwd / "assets/fonts");
    }

    bool loaded = false;
    if (const char* override_font = std::getenv("POLYMESH_GUI_FONT");
        override_font != nullptr && override_font[0] != '\0') {
        loaded = try_body(override_font);
    }
    if (!loaded) {
        for (const auto& root : brand_roots) {
            if (try_body(root / "Rubik-Regular.ttf")) {
                loaded = true;
                break;
            }
        }
    }
    if (!loaded) {
        static constexpr const char* kFallbacks[] = {
            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/arial.ttf",
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
        };
        for (const char* path : kFallbacks) {
            if (try_body(path)) {
                loaded = true;
                break;
            }
        }
    }
    if (!loaded) {
        ImFontConfig fallback;
        fallback.SizePixels = 13.0f * atlas_scale;
        body = io.Fonts->AddFontDefault(&fallback);
    }

    // Workflow headers use the branded medium weight when it is present.
    for (const auto& root : brand_roots) {
        const auto path = root / "Rubik-Medium.ttf";
        std::error_code medium_ec;
        if (!std::filesystem::is_regular_file(path, medium_ec)) {
            continue;
        }
        if (io.Fonts->AddFontFromFileTTF(path.string().c_str(), 16.0f * atlas_scale, nullptr,
                                         kRanges) != nullptr) {
            merge_symbol_glyphs(io, 16.0f * atlas_scale);
            break;
        }
    }

    if (mono_out != nullptr) {
        *mono_out = nullptr;
        for (const auto& root : brand_roots) {
            const auto path = root / "JetBrainsMono-Regular.ttf";
            std::error_code mono_ec;
            if (!std::filesystem::is_regular_file(path, mono_ec)) {
                continue;
            }
            *mono_out = io.Fonts->AddFontFromFileTTF(path.string().c_str(),
                                                     15.5f * atlas_scale, nullptr, kRanges);
            if (*mono_out != nullptr) {
                // Numerics in the live overlays and stat rows are drawn in this
                // face, and they carry η, σ and ν.
                merge_symbol_glyphs(io, 15.5f * atlas_scale);
                break;
            }
        }
        if (*mono_out == nullptr) {
            *mono_out = body;
        }
    }
    return loaded;
}

void rebuild_ui_fonts(App& app, float scale) {
    const float old_scale = ui_scale;
    if (std::abs(scale - old_scale) < 0.01f) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    io.Fonts->Clear();
    set_ui_scale(scale);
    io.FontGlobalScale = 1.0f / scale;
    apply_theme();
    app.cinema_font = nullptr;
    app.mono_font = nullptr;
    app.custom_font = load_ui_font(scale, &app.cinema_font, &app.mono_font);
    io.Fonts->Build();
    ImGui_ImplOpenGL3_CreateFontsTexture();
}

/// Parses "<w>x<h>". Returns false on anything else, including trailing junk,
/// so a typo is reported instead of silently recording at the wrong size — a
/// take mislabelled 1080p is worse than a take that never started.
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
            float r = 0.086f, g = 0.106f, b = 0.133f;                      // #161B22 cell body
            float a = body * 0.92f;
            r = r * (1.0f - ring_in) + 0.165f * ring_in; // #2A6E96
            g = g * (1.0f - ring_in) + 0.431f * ring_in;
            b = b * (1.0f - ring_in) + 0.588f * ring_in;
            a = std::max(a, ring_in);
            r = r * (1.0f - ring) + 0.298f * ring; // #4CC2FF
            g = g * (1.0f - ring) + 0.761f * ring;
            b = b * (1.0f - ring) + 1.000f * ring;
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

std::string format_legend_value(float value, const char* unit) {
    const double v = static_cast<double>(value);
    if (std::strcmp(unit, "Pa") == 0) {
        if (std::abs(v) >= 1e9) {
            return std::format("{:.3g} GPa", v / 1e9);
        }
        if (std::abs(v) >= 1e6) {
            return std::format("{:.3g} MPa", v / 1e6);
        }
        if (std::abs(v) >= 1e3) {
            return std::format("{:.3g} kPa", v / 1e3);
        }
        return std::format("{:.3g} Pa", v);
    }
    if (std::strcmp(unit, "m") == 0) {
        if (std::abs(v) >= 1.0) {
            return std::format("{:.3g} m", v);
        }
        if (std::abs(v) >= 1e-3) {
            return std::format("{:.3g} mm", v * 1e3);
        }
        if (std::abs(v) >= 1e-6) {
            return std::format("{:.3g} µm", v * 1e6);
        }
        return std::format("{:.3g} nm", v * 1e9);
    }
    return unit[0] == '\0' ? std::format("{:.3g}", v) : std::format("{:.3g} {}", v, unit);
}

} // namespace polymesh::gui
