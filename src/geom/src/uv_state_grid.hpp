// SPDX-License-Identifier: BSD-3-Clause
// Cached BRepClass point-in-face answers away from a face's boundary (OCC only).
#pragma once

#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <BndLib_Add2dCurve.hxx>
#include <Bnd_Box2d.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_Curve.hxx>
#include <Precision.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Vec2d.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace polymesh::geom::detail {

/// Point-in-face states over a UV grid, for points away from every boundary
/// curve.
///
/// BRepClass decides IN or OUT from where a ray crosses the face's pcurves
/// (BRepClass_FClassifier), and reports ON only within the classifier
/// tolerance of one (BRepClass_Intersector's CheckOn). Two points joined by a
/// segment that passes nowhere near a pcurve therefore get the same answer. A
/// grid cell that no conservative pcurve box touches is such a region: it is
/// convex, so every point in it shares the state the classifier gives at the
/// cell centre, which is computed on first use and kept.
///
/// A cell some box does touch is split 8 x 8 the first time a point lands in
/// it, against boxes of the same curves cut eight times finer, so points near
/// (but not on) the boundary are answered the same way. Sub-cells still
/// touched, points off the grid, and faces whose boundary cannot be bounded
/// conservatively always run the classifier.
class UvStateGrid {
  public:
    /// Bound every pcurve of `face` (the face the classifier walks) for a
    /// classifier tolerance `tol`, on about `cells` grid cells. Leaves the grid
    /// unusable when any boundary piece cannot be bounded conservatively.
    void build(const TopoDS_Face& face, double tol, int cells = 4096) {
        usable_ = false;
        try {
            usable_ = build_cells(face, tol, std::max(cells, 1));
        } catch (const Standard_Failure&) {
            usable_ = false;
        }
        if (!usable_) {
            state_.clear();
            curves_.clear();
            pieces_.clear();
            cell_pieces_.clear();
            fine_.clear();
        }
    }

    bool usable() const { return usable_; }

    /// The classifier's state at (u, v). `exact(gp_Pnt2d)` runs the real
    /// classifier; it is called for cell centres on first use and for every
    /// point the grid cannot answer.
    template <class Exact> TopAbs_State classify(double u, double v, Exact&& exact) {
        const double fu = (u - u0_) / du_;
        const double fv = (v - v0_) / dv_;
        if (!usable_ || !(fu >= 0.0 && fu < static_cast<double>(nu_) && fv >= 0.0 &&
                          fv < static_cast<double>(nv_))) {
            return exact(gp_Pnt2d(u, v));
        }
        const int i = std::min(static_cast<int>(fu), nu_ - 1);
        const int j = std::min(static_cast<int>(fv), nv_ - 1);
        const int cell = j * nu_ + i;
        signed char& s = state_[static_cast<std::size_t>(cell)];
        if (s == kUnknown) {
            s = known_or_boundary(exact(centre(i, j, 1, 0, 0)));
        }
        if (s != kBoundary) {
            return static_cast<TopAbs_State>(s);
        }
        // Sub-cell of the 8 x 8 refinement.
        Fine& fine = refine(cell, i, j);
        const int si = std::clamp(static_cast<int>((fu - i) * kSplit), 0, kSplit - 1);
        const int sj = std::clamp(static_cast<int>((fv - j) * kSplit), 0, kSplit - 1);
        signed char& fs = fine[static_cast<std::size_t>(sj * kSplit + si)];
        if (fs == kUnknown) {
            fs = known_or_boundary(exact(centre(i, j, kSplit, si, sj)));
        }
        if (fs != kBoundary) {
            return static_cast<TopAbs_State>(fs);
        }
        return exact(gp_Pnt2d(u, v));
    }

  private:
    static constexpr signed char kBoundary = -2;
    static constexpr signed char kUnknown = -1;
    static constexpr int kSplit = 8;
    /// Chords that estimate a pcurve's length in cells.
    static constexpr int kLengthSamples = 16;
    static constexpr int kMaxPieces = 1024;

    using Fine = std::array<signed char, kSplit * kSplit>;

    /// One boxed stretch of a pcurve, or (curve < 0) a fixed box.
    struct Piece {
        int curve = -1;
        double a = 0.0, b = 0.0;
        double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
        double pad = 0.0;
    };

    static signed char known_or_boundary(TopAbs_State s) {
        return s == TopAbs_IN || s == TopAbs_OUT ? static_cast<signed char>(s) : kBoundary;
    }

    gp_Pnt2d centre(int i, int j, int split, int si, int sj) const {
        return {u0_ + (static_cast<double>(i) + (static_cast<double>(si) + 0.5) / split) * du_,
                v0_ +
                    (static_cast<double>(j) + (static_cast<double>(sj) + 0.5) / split) * dv_};
    }

    bool build_cells(const TopoDS_Face& face, double tol, int cells) {
        Standard_Real u1 = 0.0, u2 = 0.0, v1 = 0.0, v2 = 0.0;
        BRepTools::UVBounds(face, u1, u2, v1, v2);
        const double ru = u2 - u1;
        const double rv = v2 - v1;
        if (!std::isfinite(ru) || !std::isfinite(rv) || !(ru > 0.0) || !(rv > 0.0)) {
            return false;
        }
        // CheckOn's ON band is the classifier tolerance in UV; the curve
        // domain ends carry 1e-5. Stay far outside both.
        margin_ = 16.0 * std::max(tol, 1e-5) + 1e-9 * (ru + rv);
        const double aspect = std::clamp(ru / rv, 1.0 / 64.0, 64.0);
        nu_ = std::clamp(static_cast<int>(std::lround(std::sqrt(cells * aspect))), 1, 512);
        nv_ = std::clamp(static_cast<int>(std::lround(cells / static_cast<double>(nu_))), 1,
                         512);
        u0_ = u1;
        v0_ = v1;
        du_ = ru / nu_;
        dv_ = rv / nv_;
        state_.assign(static_cast<std::size_t>(nu_) * static_cast<std::size_t>(nv_), kUnknown);

        // A vertex above the explorer's gap tolerance (0.1) makes the
        // intersector bridge its gap with a chord (CheckSkip): leave such
        // faces to the classifier.
        for (TopExp_Explorer it(face, TopAbs_VERTEX); it.More(); it.Next()) {
            if (BRep_Tool::Tolerance(TopoDS::Vertex(it.Current())) > 0.1) {
                return false;
            }
        }
        std::vector<std::pair<int, int>> cell_piece; // (cell, piece)
        for (TopExp_Explorer it(face, TopAbs_EDGE); it.More(); it.Next()) {
            const TopoDS_Edge& edge = TopoDS::Edge(it.Current());
            Standard_Real f = 0.0, l = 0.0;
            const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(edge, face, f, l);
            if (pc.IsNull() || Precision::IsInfinite(f) || Precision::IsInfinite(l) ||
                !(l > f)) {
                return false;
            }
            Geom2dAdaptor_Curve curve(pc, f, l);
            switch (curve.GetType()) {
            case GeomAbs_Line:
            case GeomAbs_Circle:
            case GeomAbs_Ellipse:
            case GeomAbs_Hyperbola:
            case GeomAbs_Parabola:
            case GeomAbs_BezierCurve:  // boxed by its poles
            case GeomAbs_BSplineCurve: // boxed by its poles
                break;
            default: // offset or other curves are only sampled by BndLib
                return false;
            }
            const int curve_index = static_cast<int>(curves_.size());
            curves_.push_back(curve);
            // About one piece per cell along the curve keeps the boundary
            // band a cell or two wide (a single box for a diagonal line would
            // mark its whole bounding rectangle). The length estimate only
            // sets the piece count; each piece is still bounded
            // conservatively.
            double length = 0.0;
            gp_Pnt2d prev = curve.Value(f);
            for (int k = 1; k <= kLengthSamples; ++k) {
                const gp_Pnt2d next = curve.Value(f + (l - f) * k / kLengthSamples);
                length += std::hypot((next.X() - prev.X()) / du_, (next.Y() - prev.Y()) / dv_);
                prev = next;
            }
            if (!std::isfinite(length)) {
                return false;
            }
            const int n_pieces =
                std::clamp(static_cast<int>(std::ceil(1.5 * length)), 1, kMaxPieces);
            for (int k = 0; k < n_pieces; ++k) {
                Piece piece;
                piece.curve = curve_index;
                piece.a = f + (l - f) * k / n_pieces;
                piece.b = k + 1 == n_pieces ? l : f + (l - f) * (k + 1) / n_pieces;
                piece.pad = margin_;
                if (!bound(curve, piece.a, piece.b, piece)) {
                    return false;
                }
                add_piece(piece, cell_piece);
            }
            // CheckOn reads the curve over [f - tol, l + tol].
            for (const double t : {f, l}) {
                gp_Pnt2d p;
                gp_Vec2d d;
                curve.D1(t, p, d);
                Piece end;
                end.pad = 2.0 * tol * d.Magnitude() + margin_;
                if (!std::isfinite(end.pad)) {
                    return false;
                }
                end.x0 = end.x1 = p.X();
                end.y0 = end.y1 = p.Y();
                add_piece(end, cell_piece);
            }
        }
        std::sort(cell_piece.begin(), cell_piece.end());
        cell_piece.erase(std::unique(cell_piece.begin(), cell_piece.end()), cell_piece.end());
        cell_pieces_ = std::move(cell_piece);
        return true;
    }

    static bool bound(const Geom2dAdaptor_Curve& curve, double a, double b, Piece& piece) {
        Bnd_Box2d box;
        BndLib_Add2dCurve::Add(curve, a, b, 0.0, box);
        if (box.IsVoid()) {
            return false;
        }
        box.Get(piece.x0, piece.y0, piece.x1, piece.y1);
        return std::isfinite(piece.x0) && std::isfinite(piece.y0) && std::isfinite(piece.x1) &&
               std::isfinite(piece.y1);
    }

    /// Cell index range [i0, i1] x [j0, j1] a padded box covers at
    /// `split` sub-divisions of cell (ci, cj) (split 1: the whole grid), or
    /// false when it misses.
    bool cover(const Piece& p, int split, int ci, int cj, int& i0, int& i1, int& j0,
               int& j1) const {
        const double x0 = p.x0 - p.pad, x1 = p.x1 + p.pad;
        const double y0 = p.y0 - p.pad, y1 = p.y1 + p.pad;
        const double ou = split == 1 ? u0_ : u0_ + ci * du_;
        const double ov = split == 1 ? v0_ : v0_ + cj * dv_;
        const double su = du_ / split, sv = dv_ / split;
        const int n_u = split == 1 ? nu_ : split, n_v = split == 1 ? nv_ : split;
        if (x1 < ou || y1 < ov || x0 > ou + n_u * su || y0 > ov + n_v * sv) {
            return false;
        }
        i0 = std::clamp(static_cast<int>(std::floor((x0 - ou) / su)), 0, n_u - 1);
        i1 = std::clamp(static_cast<int>(std::floor((x1 - ou) / su)), 0, n_u - 1);
        j0 = std::clamp(static_cast<int>(std::floor((y0 - ov) / sv)), 0, n_v - 1);
        j1 = std::clamp(static_cast<int>(std::floor((y1 - ov) / sv)), 0, n_v - 1);
        return true;
    }

    void add_piece(const Piece& piece, std::vector<std::pair<int, int>>& cell_piece) {
        int i0 = 0, i1 = 0, j0 = 0, j1 = 0;
        if (!cover(piece, 1, 0, 0, i0, i1, j0, j1)) {
            return;
        }
        const int id = static_cast<int>(pieces_.size());
        pieces_.push_back(piece);
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                const int cell = j * nu_ + i;
                state_[static_cast<std::size_t>(cell)] = kBoundary;
                cell_piece.emplace_back(cell, id);
            }
        }
    }

    /// The 8 x 8 sub-grid of boundary cell (i, j), built on first use from
    /// the pieces that touch the cell, each cut into kSplit finer pieces.
    Fine& refine(int cell, int i, int j) {
        if (auto it = fine_.find(cell); it != fine_.end()) {
            return *it->second;
        }
        auto fine = std::make_unique<Fine>();
        fine->fill(kUnknown);
        const auto mark = [&](const Piece& p) {
            int i0 = 0, i1 = 0, j0 = 0, j1 = 0;
            if (cover(p, kSplit, i, j, i0, i1, j0, j1)) {
                for (int sj = j0; sj <= j1; ++sj) {
                    for (int si = i0; si <= i1; ++si) {
                        (*fine)[static_cast<std::size_t>(sj * kSplit + si)] = kBoundary;
                    }
                }
            }
        };
        const auto range = std::equal_range(
            cell_pieces_.begin(), cell_pieces_.end(), std::pair<int, int>{cell, -1},
            [](const auto& x, const auto& y) { return x.first < y.first; });
        for (auto it = range.first; it != range.second; ++it) {
            const Piece& piece = pieces_[static_cast<std::size_t>(it->second)];
            if (piece.curve < 0) {
                mark(piece);
                continue;
            }
            bool bounded = true;
            for (int k = 0; k < kSplit && bounded; ++k) {
                Piece sub = piece;
                sub.a = piece.a + (piece.b - piece.a) * k / kSplit;
                sub.b = k + 1 == kSplit ? piece.b
                                        : piece.a + (piece.b - piece.a) * (k + 1) / kSplit;
                bounded =
                    bound(curves_[static_cast<std::size_t>(piece.curve)], sub.a, sub.b, sub);
                if (bounded) {
                    mark(sub);
                }
            }
            if (!bounded) { // keep the piece's own (coarser) box
                mark(piece);
            }
        }
        Fine& out = *fine;
        fine_.emplace(cell, std::move(fine));
        return out;
    }

    bool usable_ = false;
    int nu_ = 0, nv_ = 0;
    double u0_ = 0.0, v0_ = 0.0, du_ = 1.0, dv_ = 1.0;
    double margin_ = 0.0;
    std::vector<signed char> state_;
    std::vector<Geom2dAdaptor_Curve> curves_;
    std::vector<Piece> pieces_;
    std::vector<std::pair<int, int>> cell_pieces_; ///< sorted (cell, piece)
    std::unordered_map<int, std::unique_ptr<Fine>> fine_;
};

} // namespace polymesh::geom::detail
