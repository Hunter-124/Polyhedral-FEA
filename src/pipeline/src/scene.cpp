// SPDX-License-Identifier: BSD-3-Clause
#include "pipeline/scene.hpp"

#include "adapt/error.hpp"
#include "adapt/hp_driver.hpp"
#include "adapt/loop.hpp"
#include "fea/boundary_faces.hpp"
#include "fea/p_elevate.hpp"
#include "fea/solve.hpp"
#include "fea/vem.hpp"
#include "fea/vtu.hpp"
#include "fea/zz.hpp"
#include "geom/cad_model.hpp"
#include "geom/cad_topology.hpp"
#include "geom/features.hpp"
#include "geom/indicators.hpp"
#include "geom/step.hpp"
#include "geom/stl.hpp"
#include "mesh/grid_classify.hpp"
#include "mesh/hex_fill.hpp"
#include "mesh/hybrid_fill.hpp"
#include "mesh/local_refine.hpp"
#include "mesh/mixed_fill.hpp"
#include "mesh/octa_fill.hpp"
#include "mesh/prism_fill.hpp"
#include "mesh/quality.hpp"
#include "mesh/surface_project.hpp"
#include "mesh/tet_fill.hpp"
#include "mesh/transition_fill.hpp"
#include "mesh/cvt_export.hpp"
#include "mesh/cvt_lloyd.hpp"
#include "mesh/cvt_sites.hpp"
#include "mesh/geogram_clip.hpp"
#include "fea/poly_to_vem.hpp"
#include "mesh/varyhedron_fill.hpp"

#include <Eigen/Geometry>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <queue>
#include <set>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace polymesh::pipeline {
namespace adapt = polymesh::adapt;

namespace {

Eigen::Vector3d triangle_normal(const geom::TriSurface& s, std::size_t t) {
    const auto& tri = s.triangles[t];
    const Eigen::Vector3d ab = s.vertices[tri[1]] - s.vertices[tri[0]];
    const Eigen::Vector3d ac = s.vertices[tri[2]] - s.vertices[tri[0]];
    return ab.cross(ac).normalized();
}

/// Closest distance from a point to a triangle (Ericson, Real-Time
/// Collision Detection).
double point_triangle_distance(const Eigen::Vector3d& p, const Eigen::Vector3d& a,
                               const Eigen::Vector3d& b, const Eigen::Vector3d& c) {
    const Eigen::Vector3d ab = b - a, ac = c - a, ap = p - a;
    const double d1 = ab.dot(ap), d2 = ac.dot(ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return (p - a).norm();
    }
    const Eigen::Vector3d bp = p - b;
    const double d3 = ab.dot(bp), d4 = ac.dot(bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return (p - b).norm();
    }
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return (p - (a + ab * (d1 / (d1 - d3)))).norm();
    }
    const Eigen::Vector3d cp = p - c;
    const double d5 = ab.dot(cp), d6 = ac.dot(cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return (p - c).norm();
    }
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return (p - (a + ac * (d2 / (d2 - d6)))).norm();
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return (p - (b + (c - b) * w)).norm();
    }
    const double denom = 1.0 / (va + vb + vc);
    const Eigen::Vector3d closest = a + ab * (vb * denom) + ac * (vc * denom);
    return (p - closest).norm();
}

} // namespace

Model Model::load(const std::string& path, double sharp_angle_deg) {
    Model model;
    const auto lower = [&] {
        std::string s = path;
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    model.source_path = path;
    const auto slash = path.find_last_of("/\\");
    model.name = slash == std::string::npos ? path : path.substr(slash + 1);

    // ADR-0020: STEP/BREP retain CadModel; tessellation is derived for regions
    // and legacy hybrid fill. STL stays surface-only (compare/legacy).
    if (lower.ends_with(".step") || lower.ends_with(".stp")) {
        model.cad = geom::CadModel::load_step(path);
        model.surface = model.cad->tessellate();
        model.bbox_min = model.cad->bbox_min();
        model.bbox_max = model.cad->bbox_max();
    } else if (lower.ends_with(".brep") || lower.ends_with(".brp")) {
        model.cad = geom::CadModel::load_brep(path);
        model.surface = model.cad->tessellate();
        model.bbox_min = model.cad->bbox_min();
        model.bbox_max = model.cad->bbox_max();
    } else {
        model.surface = geom::load_stl(path);
        model.bbox_min = model.surface.vertices.front();
        model.bbox_max = model.surface.vertices.front();
        for (const auto& v : model.surface.vertices) {
            model.bbox_min = model.bbox_min.cwiseMin(v);
            model.bbox_max = model.bbox_max.cwiseMax(v);
        }
    }
    model.surface.validate();

    // CAD-style face regions: grow across edges whose dihedral angle is
    // below the sharp threshold.
    const std::size_t n_tris = model.surface.triangles.size();
    std::vector<Eigen::Vector3d> normals(n_tris);
    for (std::size_t t = 0; t < n_tris; ++t) {
        normals[t] = triangle_normal(model.surface, t);
    }
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint32_t>> edge_tris;
    for (std::size_t t = 0; t < n_tris; ++t) {
        const auto& tri = model.surface.triangles[t];
        for (int e = 0; e < 3; ++e) {
            const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                         tri[static_cast<std::size_t>((e + 1) % 3)]);
            edge_tris[key].push_back(static_cast<std::uint32_t>(t));
        }
    }
    const double cos_sharp = std::cos(sharp_angle_deg * std::numbers::pi / 180.0);
    model.triangle_region.assign(n_tris, -1);
    for (std::size_t seed = 0; seed < n_tris; ++seed) {
        if (model.triangle_region[seed] >= 0) {
            continue;
        }
        const int region = model.region_count++;
        std::queue<std::uint32_t> frontier;
        frontier.push(static_cast<std::uint32_t>(seed));
        model.triangle_region[seed] = region;
        while (!frontier.empty()) {
            const auto t = frontier.front();
            frontier.pop();
            const auto& tri = model.surface.triangles[t];
            for (int e = 0; e < 3; ++e) {
                const auto key = std::minmax(tri[static_cast<std::size_t>(e)],
                                             tri[static_cast<std::size_t>((e + 1) % 3)]);
                for (const auto other : edge_tris.at(key)) {
                    if (model.triangle_region[other] >= 0) {
                        continue;
                    }
                    if (normals[t].dot(normals[other]) > cos_sharp) {
                        model.triangle_region[other] = region;
                        frontier.push(other);
                    }
                }
            }
        }
    }
    return model;
}

ResolvedMeshSize resolve_mesh_size(const Model& model, double requested_h,
                                   double sharp_angle_deg) {
    ResolvedMeshSize out;
    if (requested_h > 0.0) {
        out.h = requested_h;
        out.auto_chosen = false;
        out.note = std::format("h={:.4g} m (user)", out.h);
        return out;
    }

    const Eigen::Vector3d extent_vec = model.bbox_max - model.bbox_min;
    const double extent = extent_vec.maxCoeff();
    const double diagonal = extent_vec.norm();
    // Primary scale: max edge of AABB / 16 (practical zero-tune; former CLI
    // mesh default). Secondary: diagonal / 28 — keeps long thin parts from
    // exploding along the short axes while still resolving the long span.
    double h_geom = extent / 16.0;
    if (diagonal > 0.0) {
        h_geom = std::min(h_geom, diagonal / 28.0);
    }
    if (!(h_geom > 0.0) || !std::isfinite(h_geom)) {
        h_geom = 0.05; // last-resort fallback for degenerate bbox
    }

    const auto edges = geom::detect_sharp_edges(model.surface, sharp_angle_deg);
    out.n_sharp_edges = edges.size();
    // Prefer geometric feature scale over STL facet edge length: a faceted hole
    // has hundreds of short creases that used to drive global h → million-elem floods.
    double min_feature = std::numeric_limits<double>::infinity();
    for (const auto& e : edges) {
        const double len =
            (model.surface.vertices[e.v0] - model.surface.vertices[e.v1]).norm();
        if (len > 0.02 * extent) { // ignore facet-scale creases
            min_feature = std::min(min_feature, len);
        }
    }
    // Curvature radius proxy: R ≈ 1/κ for high-κ verts (holes/fillets).
    double r_curv = std::numeric_limits<double>::infinity();
    {
        const auto curv = geom::estimate_vertex_curvature(model.surface);
        for (double k : curv.kappa) {
            if (k > 1e-9) {
                r_curv = std::min(r_curv, 1.0 / k);
            }
        }
    }
    // Thickness: thin plates need a few elements through thickness, not global flood.
    double t_min = std::numeric_limits<double>::infinity();
    {
        const auto thick = geom::estimate_local_thickness(model.surface);
        for (double t : thick.thickness) {
            if (geom::has_finite_thickness(t) && t > 1e-12) {
                t_min = std::min(t_min, t);
            }
        }
    }
    if (!std::isfinite(min_feature)) {
        min_feature = 0.0;
    }
    out.min_feature_length = min_feature;

    // Mild density tweak only — never the old dens×0.55 from facet count.
    double density_scale = 1.0;
    if (out.n_sharp_edges > 40 && out.n_sharp_edges <= 120) {
        density_scale = 0.92;
    } else if (out.n_sharp_edges > 120) {
        density_scale = 0.88; // faceted curves: keep bulk coarse; local LEB refines
    }

    double h0 = h_geom * density_scale;
    // Resolve feature geometry with local multi-level LEB, not global h collapse.
    // Aim ~5–6 bulk cells across characteristic R so L2 (~h/4) yields a smooth hole.
    if (std::isfinite(r_curv) && r_curv > 0.0) {
        // ~6 bulk cells across characteristic radius; L2 LEB densifies the rim further.
        const double h_r = r_curv / 6.0;
        if (h_r < h0) {
            h0 = std::max(h_r, h_geom * 0.28);
        }
    }
    if (std::isfinite(t_min) && t_min > 0.0) {
        const double h_t = t_min / 2.0;
        if (h_t < h0 && h_t > h_geom * 0.2) {
            h0 = std::min(h0, std::max(h_t, h_geom * 0.35));
        }
    }
    if (min_feature > 0.0) {
        const double h_feat = 0.35 * min_feature;
        if (h_feat < h0) {
            h0 = std::max(h_feat, h_geom * 0.4);
        }
    }

    // BRep edge lengths (ADR-0020 / V1c): prefer retained Model::cad; fall back
    // to reloading source_path for surface-only models that still have a CAD path.
    double cad_min_edge = std::numeric_limits<double>::infinity();
    if (geom::occ_enabled()) {
        try {
            std::optional<geom::CadModel> cad_owned;
            const geom::CadModel* cad_ptr = nullptr;
            if (model.cad && !model.cad->empty()) {
                cad_ptr = &(*model.cad);
            } else if (!model.source_path.empty()) {
                cad_owned = geom::load_cad(model.source_path);
                if (cad_owned && !cad_owned->empty()) {
                    cad_ptr = &(*cad_owned);
                }
            }
            if (cad_ptr != nullptr) {
                const geom::CadTopology topo = geom::extract_topology(*cad_ptr, 4);
                for (const auto& e : topo.edges) {
                    if (e.length > 1e-12 && e.length > 0.02 * extent) {
                        cad_min_edge = std::min(cad_min_edge, e.length);
                    }
                }
                // Hole / fillet arcs often appear as single short edges relative
                // to bbox; still honor them if longer than 1% extent.
                if (!std::isfinite(cad_min_edge)) {
                    for (const auto& e : topo.edges) {
                        if (e.length > 0.01 * extent) {
                            cad_min_edge = std::min(cad_min_edge, e.length);
                        }
                    }
                }
            }
        } catch (...) {
            // Surface-only auto-h fallback.
        }
    }
    if (std::isfinite(cad_min_edge) && cad_min_edge > 0.0) {
        out.min_feature_length =
            (out.min_feature_length > 0.0)
                ? std::min(out.min_feature_length, cad_min_edge)
                : cad_min_edge;
        const double h_cad = 0.3 * cad_min_edge;
        if (h_cad < h0) {
            h0 = std::max(h_cad, h_geom * 0.35);
        }
    }

    // Absolute clamps: coarser floor than before so interactive meshes stay sane.
    if (diagonal > 0.0) {
        h0 = std::clamp(h0, diagonal / 80.0, diagonal / 6.0);
    }
    out.h = h0;
    out.auto_chosen = true;
    out.note = std::format(
        "auto h={:.4g} m (extent/16∩diag/28, n_sharp={}, min_feat={:.3g} m, dens×{:.2f}"
        "{}{})",
        out.h, out.n_sharp_edges, out.min_feature_length, density_scale,
        std::isfinite(r_curv) ? std::format(", Rκ≈{:.3g}", r_curv) : std::string{},
        std::isfinite(cad_min_edge) ? std::format(", CAD_edge≈{:.3g}", cad_min_edge)
                                    : std::string{});
    return out;
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

VolumeMeshOutput volume_mesh(const Model& model, double h, VolumeMesher mesher,
                             int skin_layers, bool feature_refine,
                             std::span<const Eigen::Vector3d> refine_seeds, double seed_band,
                             double element_tendency) {
    VolumeMeshOutput out;
    double fill_h = h;
    const auto tendency_plan = resolve_element_tendency(mesher, element_tendency, skin_layers);
    mesher = tendency_plan.mesher;
    skin_layers = tendency_plan.skin_layers;
    // Per-cell turning-angle refinement threshold (feature_refine paths): refine
    // where the surface turns more than this per bulk cell (h·κ). Angle-based,
    // so gentle curves / big bores stay coarse and flats never refine — replaces
    // the capped seed-ball scheme (coarse rings mid-bore, fine islands on flats).
    constexpr double kCurvatureTurnDeg = 15.0;
    if (mesher == VolumeMesher::kHybrid || mesher == VolumeMesher::kHybridVem) {
        // SPEC hybrid zoo: hex bulk @ h + 2:1 fine @ h/2 on feature/curvature
        // bands + conforming transitions.
        // kHybrid: product FE expands hex→pyramids (ADR-0012 / ADR-0013).
        // kHybridVem: keep hex as FE + unsplit transition polyhedra as VEM
        // (ADR-0019 fe-vem-assembly); no fan-split, no hex→pyramid expand.
        const bool native_poly = (mesher == VolumeMesher::kHybridVem);
        std::vector<geom::SharpEdge> edges;
        std::vector<Eigen::Vector3d> adapt_seeds(refine_seeds.begin(), refine_seeds.end());
        double feat_band = 0.0;
        double s_band = seed_band;
        double turn_deg = 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                // Feature band ~2 bulk cells so hole rims get a clear h/2 shell.
                feat_band = 2.0 * h;
            }
            turn_deg = kCurvatureTurnDeg;
        }
        // A-posteriori adapt seeds (caller) keep their ball semantics.
        if (s_band <= 0.0 && !adapt_seeds.empty()) {
            s_band = 2.0 * h;
        }
        if (adapt_seeds.empty()) {
            s_band = 0.0;
        }
        // Build lattice without snap first; product FE snaps after hex→pyramid
        // expand so free-surface Jacobian is pyramid-based. Native-poly path
        // keeps hex FE + poly VEM and snaps on that mesh.
        auto raw = mesh::mixed_fill_surface(model.surface, model.bbox_min, model.bbox_max, h,
                                            std::max(1, skin_layers), edges, feat_band,
                                            adapt_seeds, s_band, /*snap_boundary=*/false,
                                            turn_deg, native_poly);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_raw = raw.n_pyramid;
        const std::size_t n_poly_raw = raw.n_poly;
        auto fill = native_poly ? std::move(raw) : mesh::expand_mixed_hex_to_pyramids(raw);
        fill_h = fill.h;
        // Post-expand free-surface snap (boundary quads from lattice).
        if (!fill.boundary_quads.empty()) {
            std::set<std::uint32_t> bset;
            for (const auto& q : fill.boundary_quads) {
                bset.insert(q.begin(), q.end());
            }
            std::vector<std::uint32_t> bnodes(bset.begin(), bset.end());
            const double h_snap = fill.h > 0.0 ? fill.h : h;
            const double vol_eps = 1e-14 * h_snap * h_snap * h_snap;
            // Fan tets must keep a usable shape: the snap used to flatten them
            // unchecked (only pyramids were guarded), leaving zero-aspect
            // boundary tets in the product mesh (M6 → 0 at fine h).
            constexpr double kMinTetAspect = 0.02;
            const auto tet_aspect_ok = [&](const mesh::MixedCell& cell) {
                const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                const double v = (b - a).dot((c - a).cross(d - a)) / 6.0;
                if (v <= vol_eps) {
                    return false;
                }
                const double emax = std::max({(a - b).norm(), (a - c).norm(), (a - d).norm(),
                                              (b - c).norm(), (b - d).norm(), (c - d).norm()});
                if (emax <= 0.0) {
                    return false;
                }
                return 6.0 * 1.4142135623730951 * v / (emax * emax * emax) >= kMinTetAspect;
            };
            fill.boundary_max_distance =
                mesh::snap_boundary_nodes(
                    model.surface, fill.nodes, bnodes, h_snap,
                    [&](std::set<std::uint32_t>& offenders) {
                        for (const auto& cell : fill.cells) {
                            if (cell.kind == mesh::MixedCellKind::kTet4) {
                                // Volume only: a fan tet flattened by a full
                                // snap is peeled right after (apex coplanar) —
                                // cheaper than unsnapping the wall.
                                const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                                const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                                const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                                const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                                if ((b - a).dot((c - a).cross(d - a)) < 0.0) {
                                    offenders.insert(cell.nodes.begin(),
                                                     cell.nodes.begin() + cell.n_nodes);
                                }
                                continue;
                            }
                            if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                                std::vector<Eigen::Vector3d> coords;
                                coords.reserve(cell.poly_nodes.size());
                                for (const auto g : cell.poly_nodes) {
                                    coords.push_back(fill.nodes[g]);
                                }
                                if (fea::poly_volume(coords, cell.poly_faces) <= vol_eps) {
                                    offenders.insert(cell.poly_nodes.begin(),
                                                     cell.poly_nodes.end());
                                }
                                continue;
                            }
                            if (cell.kind == mesh::MixedCellKind::kHex8) {
                                // Native-poly path: reject inverted hex free faces.
                                continue; // hex8 Jacobian checked via cell_valid later
                            }
                            if (cell.kind != mesh::MixedCellKind::kPyramid5) {
                                continue;
                            }
                            const double v1 =
                                ((fill.nodes[cell.nodes[1]] - fill.nodes[cell.nodes[0]])
                                     .dot((fill.nodes[cell.nodes[2]] -
                                           fill.nodes[cell.nodes[0]])
                                              .cross(fill.nodes[cell.nodes[4]] -
                                                     fill.nodes[cell.nodes[0]]))) /
                                6.0;
                            const double v2 =
                                ((fill.nodes[cell.nodes[2]] - fill.nodes[cell.nodes[0]])
                                     .dot((fill.nodes[cell.nodes[3]] -
                                           fill.nodes[cell.nodes[0]])
                                              .cross(fill.nodes[cell.nodes[4]] -
                                                     fill.nodes[cell.nodes[0]]))) /
                                6.0;
                            if (std::abs(v1) <= vol_eps || std::abs(v2) <= vol_eps) {
                                offenders.insert(cell.nodes.begin(),
                                                 cell.nodes.begin() + cell.n_nodes);
                            }
                        }
                    },
                    /*max_move_frac=*/1.25, /*passes=*/8, edges)
                    .max_residual;
            // Peel snap-flattened fan tets: a full wall snap can pull all three
            // free nodes of a transition fan tet into the apex plane (aspect →
            // 0). The apex is then coplanar with the wall, so deleting the tet
            // exposes conforming faces with ~zero residual — better than
            // unsnapping the wall to save a degenerate element.
            {
                struct TriKey {
                    std::uint32_t a, b, c;
                    bool operator==(const TriKey& o) const {
                        return a == o.a && b == o.b && c == o.c;
                    }
                };
                struct TriHash {
                    std::size_t operator()(const TriKey& f) const noexcept {
                        std::size_t s = f.a;
                        s ^= static_cast<std::size_t>(f.b) + 0x9e3779b97f4a7c15ULL +
                             (s << 6) + (s >> 2);
                        s ^= static_cast<std::size_t>(f.c) + 0x9e3779b97f4a7c15ULL +
                             (s << 6) + (s >> 2);
                        return s;
                    }
                };
                const auto tkey = [](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
                    std::array<std::uint32_t, 3> v{{x, y, z}};
                    std::sort(v.begin(), v.end());
                    return TriKey{v[0], v[1], v[2]};
                };
                static constexpr int kTetTris[4][3] = {
                    {0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
                static constexpr int kPyrTris[4][3] = {
                    {0, 1, 4}, {1, 2, 4}, {2, 3, 4}, {3, 0, 4}};
                const auto count_tris = [&]() {
                    std::unordered_map<TriKey, int, TriHash> tri_count;
                    tri_count.reserve(fill.cells.size() * 2);
                    for (const auto& cell : fill.cells) {
                        const auto& tt =
                            (cell.kind == mesh::MixedCellKind::kTet4) ? kTetTris : kPyrTris;
                        for (int f = 0; f < 4; ++f) {
                            ++tri_count[tkey(cell.nodes[static_cast<std::size_t>(tt[f][0])],
                                             cell.nodes[static_cast<std::size_t>(tt[f][1])],
                                             cell.nodes[static_cast<std::size_t>(tt[f][2])])];
                        }
                    }
                    return tri_count;
                };
                bool peeled_any = false;
                for (int pass = 0; pass < 3; ++pass) {
                    const auto tri_count = count_tris();
                    std::vector<char> kill(fill.cells.size(), 0);
                    std::size_t n_kill = 0;
                    for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                        const auto& cell = fill.cells[ci];
                        if (cell.kind != mesh::MixedCellKind::kTet4 || tet_aspect_ok(cell)) {
                            continue;
                        }
                        for (const auto& f : kTetTris) {
                            const auto it = tri_count.find(
                                tkey(cell.nodes[static_cast<std::size_t>(f[0])],
                                     cell.nodes[static_cast<std::size_t>(f[1])],
                                     cell.nodes[static_cast<std::size_t>(f[2])]));
                            if (it != tri_count.end() && it->second == 1) {
                                kill[ci] = 1;
                                ++n_kill;
                                break;
                            }
                        }
                    }
                    if (n_kill == 0 || n_kill >= fill.cells.size()) {
                        break;
                    }
                    std::size_t w = 0;
                    for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                        if (!kill[ci]) {
                            fill.cells[w++] = fill.cells[ci];
                        }
                    }
                    fill.cells.resize(w);
                    fill.n_tet -= n_kill;
                    peeled_any = true;
                }
                if (peeled_any) {
                    // Rebuild tri-encoded boundary entries from the surviving
                    // cells (true quads — pyramid bases — are unaffected by a
                    // tet peel and are kept as-is).
                    std::erase_if(fill.boundary_quads,
                                  [](const auto& q) { return q[2] == q[3]; });
                    const auto tri_count = count_tris();
                    for (const auto& cell : fill.cells) {
                        const auto& tt =
                            (cell.kind == mesh::MixedCellKind::kTet4) ? kTetTris : kPyrTris;
                        for (int f = 0; f < 4; ++f) {
                            const auto n0 = cell.nodes[static_cast<std::size_t>(tt[f][0])];
                            const auto n1 = cell.nodes[static_cast<std::size_t>(tt[f][1])];
                            const auto n2 = cell.nodes[static_cast<std::size_t>(tt[f][2])];
                            const auto it = tri_count.find(tkey(n0, n1, n2));
                            if (it != tri_count.end() && it->second == 1) {
                                fill.boundary_quads.push_back({{n0, n1, n2, n2}});
                            }
                        }
                    }
                }
            }
            // Per-node outlier re-projection (mirror of graded S3): residual
            // stragglers get a full/partial projection accepted only when every
            // incident cell stays valid. After expand all cells are pyramid/tet.
            std::unordered_map<std::uint32_t, std::vector<std::size_t>> node_cells;
            for (std::size_t ci = 0; ci < fill.cells.size(); ++ci) {
                const auto& cell = fill.cells[ci];
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    for (const auto g : cell.poly_nodes) {
                        node_cells[g].push_back(ci);
                    }
                } else {
                    for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                        node_cells[cell.nodes[m]].push_back(ci);
                    }
                }
            }
            const auto cell_valid = [&](const mesh::MixedCell& cell) {
                if (cell.kind == mesh::MixedCellKind::kTet4) {
                    return tet_aspect_ok(cell);
                }
                if (cell.kind == mesh::MixedCellKind::kPyramid5) {
                    const Eigen::Vector3d& a = fill.nodes[cell.nodes[0]];
                    const Eigen::Vector3d& b = fill.nodes[cell.nodes[1]];
                    const Eigen::Vector3d& c = fill.nodes[cell.nodes[2]];
                    const Eigen::Vector3d& d = fill.nodes[cell.nodes[3]];
                    const Eigen::Vector3d& e = fill.nodes[cell.nodes[4]];
                    const double v1 = (b - a).dot((c - a).cross(e - a)) / 6.0;
                    const double v2 = (c - a).dot((d - a).cross(e - a)) / 6.0;
                    return std::abs(v1) > vol_eps && std::abs(v2) > vol_eps;
                }
                if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                    std::vector<Eigen::Vector3d> coords;
                    coords.reserve(cell.poly_nodes.size());
                    for (const auto g : cell.poly_nodes) {
                        coords.push_back(fill.nodes[g]);
                    }
                    return fea::poly_volume(coords, cell.poly_faces) > vol_eps;
                }
                return true;
            };
            const double thr = 0.08 * h_snap;
            double max_resid = 0.0;
            for (const auto ni : bnodes) {
                if (ni >= fill.nodes.size()) {
                    continue;
                }
                const auto cp = mesh::closest_on_surface(model.surface, fill.nodes[ni]);
                double resid = cp.distance;
                if (resid > thr && resid <= 2.5 * h_snap) {
                    const Eigen::Vector3d saved = fill.nodes[ni];
                    static constexpr double kFracs[] = {1.0, 0.6, 0.35};
                    for (const double frac : kFracs) {
                        fill.nodes[ni] = saved + frac * (cp.point - saved);
                        bool ok = true;
                        const auto it = node_cells.find(ni);
                        if (it != node_cells.end()) {
                            for (const auto ci : it->second) {
                                if (!cell_valid(fill.cells[ci])) {
                                    ok = false;
                                    break;
                                }
                            }
                        }
                        if (ok) {
                            resid = (1.0 - frac) * resid;
                            break;
                        }
                        fill.nodes[ni] = saved;
                    }
                }
                max_resid = std::max(max_resid, resid);
            }
            fill.boundary_max_distance = max_resid;
            // Tangential smoothing: even out lattice-stair spacing on curved
            // walls / hole rims (crease nodes relax along the crease). Moves
            // are re-projected so the residual cannot grow; any move that
            // invalidates a cell is reverted.
            const auto smooth_st = mesh::smooth_boundary_nodes(
                model.surface, fill.nodes, fill.boundary_quads, h_snap,
                [&](std::set<std::uint32_t>& offenders) {
                    for (const auto& cell : fill.cells) {
                        if (cell_valid(cell)) {
                            continue;
                        }
                        if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                            offenders.insert(cell.poly_nodes.begin(), cell.poly_nodes.end());
                        } else {
                            for (std::uint8_t m = 0; m < cell.n_nodes; ++m) {
                                offenders.insert(cell.nodes[m]);
                            }
                        }
                    }
                },
                /*passes=*/3, /*relax=*/0.5, edges);
            if (smooth_st.n_moved > 0) {
                fill.boundary_max_distance = smooth_st.max_residual;
            }
        }
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        for (const auto& cell : fill.cells) {
            if (cell.kind == mesh::MixedCellKind::kPolyVem) {
                out.mesh.elements.emplace_back(fea::ElementType::kPolyVem, cell.poly_nodes,
                                               cell.poly_faces);
            } else if (cell.kind == mesh::MixedCellKind::kPyramid5) {
                out.mesh.elements.push_back(
                    fea::NodalElement{fea::ElementType::kPyramid5,
                                      {cell.nodes[0], cell.nodes[1], cell.nodes[2],
                                       cell.nodes[3], cell.nodes[4]}});
            } else if (cell.kind == mesh::MixedCellKind::kHex8) {
                out.mesh.elements.push_back(fea::NodalElement{
                    fea::ElementType::kHex8,
                    {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4],
                     cell.nodes[5], cell.nodes[6], cell.nodes[7]}});
            } else {
                out.mesh.elements.push_back(fea::NodalElement{
                    fea::ElementType::kTet4,
                    {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3]}});
            }
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        if (native_poly) {
            out.mesher_note = std::format(
                "hybrid-VEM zoo (hex FE bulk@h + 2:1 fine@h/2 + native poly VEM "
                "transitions; not Delaunay): {} hex8 + {} polyVEM ({} pyr raw unused), "
                "{} nodes, h_bulk={:.4g}/h_fine={:.4g} m, fine_cells={} transition={} "
                "feature={}, snap max|d|={:.3g} m{}",
                fill.n_hex, fill.n_poly, n_pyr_raw, out.mesh.nodes.size(), fill.h,
                fill.h_fine > 0.0 ? fill.h_fine : fill.h, fill.n_fine_cells,
                fill.n_transition_cells, fill.n_feature_skin_cells, fill.boundary_max_distance,
                turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                               : std::string{});
            (void)n_hex_lattice;
            (void)n_poly_raw;
        } else {
            out.mesher_note = std::format(
                "hybrid zoo v4 (hex bulk@h + 2:1 fine@h/2 + conforming fan transition; "
                "not Delaunay): {} hex + {} pyr raw → {} pyramid5 + {} tet4, {} nodes, "
                "h_bulk={:.4g}/h_fine={:.4g} m, fine_cells={} transition={} feature={}, "
                "snap max|d|={:.3g} m{}",
                n_hex_lattice, n_pyr_raw, fill.n_pyramid, fill.n_tet, out.mesh.nodes.size(),
                fill.h, fill.h_fine > 0.0 ? fill.h_fine : fill.h, fill.n_fine_cells,
                fill.n_transition_cells, fill.n_feature_skin_cells, fill.boundary_max_distance,
                turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                               : std::string{});
        }
    } else if (mesher == VolumeMesher::kHexFill || mesher == VolumeMesher::kHexVem) {
        auto fill = mesh::hex_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.hexes.size());
        for (const auto& hx : fill.hexes) {
            fea::NodalElement el{fea::ElementType::kHex8,
                                 {hx[0], hx[1], hx[2], hx[3], hx[4], hx[5], hx[6], hx[7]}};
            if (mesher == VolumeMesher::kHexVem) {
                auto poly = fea::hex8_as_poly(el);
                el.type = fea::ElementType::kPolyVem;
                el.faces = std::move(poly.faces);
            }
            out.mesh.elements.push_back(std::move(el));
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "{} grid fill v1 (Cartesian, not CAD-fitted): {} cells, {} nodes, h={:.4g} m, "
            "snap max|d|={:.3g} m",
            mesher == VolumeMesher::kHexVem ? "hex-VEM" : "hex", out.mesh.elements.size(),
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kHexPyramid) {
        // Topology: hex core + pyramid skin (ADR-0013). Product FE path expands
        // each interior hex to six pyramids (centroid apex) so face diagonals
        // match the tet-split pyramid skin — constant-strain patch exact.
        auto raw =
            mesh::transition_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        const std::size_t n_hex_lattice = raw.n_hex;
        const std::size_t n_pyr_skin = raw.n_pyramid;
        auto fill = mesh::expand_hex_core_to_pyramids(raw);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.cells.size());
        for (const auto& cell : fill.cells) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPyramid5,
                {cell.nodes[0], cell.nodes[1], cell.nodes[2], cell.nodes[3], cell.nodes[4]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        out.mesher_note = std::format(
            "hex+pyramid product FE (Cartesian grid, not Delaunay; all-pyramid expand): "
            "{} lattice hex → {} pyramids ({} skin + {} from hex), {} nodes, h={:.4g} m, "
            "boundary max|d|={:.3g} m",
            n_hex_lattice, fill.n_pyramid, n_pyr_skin, fill.n_pyramid - n_pyr_skin,
            out.mesh.nodes.size(), fill_h, fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kPrismSweep) {
        // Cartesian prism6 wedges along the longest bbox axis (ROADMAP C3).
        // Not CAD extrusion detection — same grid-fill honesty as tet/hex (ADR-0015).
        auto fill = mesh::prism_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.prisms.size());
        for (const auto& pr : fill.prisms) {
            out.mesh.elements.push_back(fea::NodalElement{
                fea::ElementType::kPrism6, {pr[0], pr[1], pr[2], pr[3], pr[4], pr[5]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        static constexpr const char* kAxis[] = {"x", "y", "z"};
        const char* axis_name =
            (fill.sweep_axis >= 0 && fill.sweep_axis < 3) ? kAxis[fill.sweep_axis] : "?";
        out.mesher_note =
            std::format("prism sweep grid fill v1 (Cartesian, not CAD extrusion; sweep={}): "
                        "{} prism6, {} nodes, h={:.4g} m, snap max|d|={:.3g} m",
                        axis_name, out.mesh.elements.size(), out.mesh.nodes.size(), fill_h,
                        fill.boundary_max_distance);
    } else if (mesher == VolumeMesher::kGradedTet) {
        std::vector<geom::SharpEdge> edges;
        double feature_band = 0.0;
        // Caller a-posteriori adapt seeds keep ball semantics; curvature is now
        // the per-cell turning-angle criterion inside the fill (no caps, no
        // stray islands). Thin walls (t < 2.5 h) still seed *locally* — but a
        // globally thin part (most of the surface thin) is an h problem, not a
        // local feature: seeding it would just scatter capped fine islands.
        std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
        double band = seed_band;
        double turn_deg = 0.0;
        std::size_t n_thin_seeds = 0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                // Crease band ~ two bulk cells so hole rims get a clear L1/L2 shell.
                feature_band = 2.0 * h;
            }
            turn_deg = kCurvatureTurnDeg;
            const auto thick = geom::estimate_local_thickness(model.surface);
            std::vector<Eigen::Vector3d> thin_pts;
            std::size_t n_finite = 0;
            for (std::size_t i = 0; i < model.surface.vertices.size(); ++i) {
                if (i >= thick.thickness.size() ||
                    !geom::has_finite_thickness(thick.thickness[i])) {
                    continue;
                }
                ++n_finite;
                if (thick.thickness[i] < 2.5 * h) {
                    thin_pts.push_back(model.surface.vertices[i]);
                }
            }
            const bool globally_thin =
                n_finite > 0 &&
                thin_pts.size() * 3 > model.surface.vertices.size(); // > ~1/3 thin
            if (!globally_thin && !thin_pts.empty()) {
                if (band <= 0.0) {
                    band = 1.6 * h;
                }
                // Spatial thinning: min sep 0.75 h, capped, caller seeds first.
                constexpr std::size_t kMaxGeoSeeds = 256;
                const double min_sep2 = (0.75 * h) * (0.75 * h);
                for (const auto& p : thin_pts) {
                    if (seeds.size() >= kMaxGeoSeeds) {
                        break;
                    }
                    bool far = true;
                    for (const auto& q : seeds) {
                        if ((p - q).squaredNorm() < min_sep2) {
                            far = false;
                            break;
                        }
                    }
                    if (far) {
                        seeds.push_back(p);
                        ++n_thin_seeds;
                    }
                }
            }
            // Flat solids: free-surface skin alone is enough (no fake seed flood).
            if (band > 0.0 && seeds.empty()) {
                band = 0.0;
            }
        }
        if (band <= 0.0 && !seeds.empty()) {
            band = 1.6 * h;
        }
        auto graded = mesh::graded_tet_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feature_band, seeds, band, turn_deg);
        fill_h = graded.h_fine;
        out.mesh.nodes = std::move(graded.mesh.nodes);
        out.mesh.elements.reserve(graded.mesh.tets.size());
        for (const auto& tet : graded.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        // Prefer true exterior faces after LEB (includes mid-edge free-surface nodes).
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        if (out.boundary_quads.empty()) {
            out.boundary_quads = std::move(graded.mesh.boundary_quads);
        }
        std::vector<std::uint32_t> bnodes;
        for (const auto& q : out.boundary_quads) {
            bnodes.insert(bnodes.end(), q.begin(), q.end());
        }
        std::sort(bnodes.begin(), bnodes.end());
        bnodes.erase(std::unique(bnodes.begin(), bnodes.end()), bnodes.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, bnodes);
        const char* budget_note =
            (graded.h_fine > (h / static_cast<double>(graded.subdivision)) * 1.05)
                ? ", h raised to cell budget"
                : "";
        out.mesher_note = std::format(
            "graded tet v6 (multi-level LEB geo + cap collapse/void carve): {} tets ({} bulk, "
            "{} refined L1/L2, "
            "{} feature, {} seed), h_bulk={:.4g}/h_L2~{:.4g} m (L0/L1/L2), "
            "snap max|d|={:.3g} m mean|d|={:.3g} m"
            "{}{}{}",
            out.mesh.elements.size(), graded.n_coarse_cells, graded.n_fine_cells,
            graded.n_feature_cells, graded.n_seed_cells, graded.h_coarse, graded.h_fine,
            conf.max_distance, conf.mean_distance, budget_note,
            turn_deg > 0.0 ? std::format(", curv_turn≤{:.0f}°/cell", turn_deg)
                           : std::string{},
            n_thin_seeds > 0 ? std::format(", thin_seeds={}", n_thin_seeds) : std::string{});
    } else if (mesher == VolumeMesher::kOctahedral) {
        // Experimental BCC octahedra → tet4 (ADR-0019). Not a product claim.
        auto fill = mesh::octa_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.mesh.nodes);
        out.mesh.elements.reserve(fill.mesh.tets.size());
        for (const auto& tet : fill.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.mesh.boundary_quads);
        out.mesher_note =
            std::format("octahedral experimental (BCC face-octa → tet4; not product): "
                        "{} tets ({} octa + {} bdy pyr), {} nodes, h={:.4g} m",
                        out.mesh.elements.size(), fill.n_octahedra, fill.n_boundary_pyramids,
                        out.mesh.nodes.size(), fill_h);
    } else if (mesher == VolumeMesher::kVaryhedron) {
        // ADR-0021 v1: CAD edge seeds + graded scaffold + edge-profile snap.
        std::vector<geom::SharpEdge> edges;
        double feature_band = 0.0;
        std::vector<Eigen::Vector3d> seeds(refine_seeds.begin(), refine_seeds.end());
        double band = seed_band;
        double turn_deg = feature_refine ? 15.0 : 0.0;
        if (feature_refine) {
            edges = geom::detect_sharp_edges(model.surface, 30.0);
            if (!edges.empty()) {
                feature_band = 2.0 * h;
            }
            if (band <= 0.0 && !seeds.empty()) {
                band = 1.6 * h;
            }
        }
        if (band <= 0.0 && !seeds.empty()) {
            band = 1.6 * h;
        }

        // Prefer retained Model::cad (ADR-0020 / V1c); fall back to reloading
        // the source CAD path when the model was surface-only (legacy).
        std::optional<geom::CadModel> cad_reload;
        std::optional<geom::CadTopology> topo;
        const geom::CadTopology* topo_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            try {
                topo = geom::extract_topology(*model.cad, 10);
                topo_ptr = &(*topo);
            } catch (...) {
                topo.reset();
                topo_ptr = nullptr;
            }
        } else if (geom::occ_enabled()) {
            std::vector<std::string> candidates;
            if (!model.source_path.empty()) {
                candidates.push_back(model.source_path);
            }
            if (!model.name.empty()) {
                candidates.push_back(model.name);
                candidates.push_back(std::string("tests/fixtures/parts/") + model.name);
                candidates.push_back(std::string("tests/fixtures/") + model.name);
            }
            for (const auto& cand : candidates) {
                try {
                    std::string low = cand;
                    for (char& c : low) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    if (low.ends_with(".step") || low.ends_with(".stp")) {
                        cad_reload = geom::CadModel::load_step(cand);
                    } else if (low.ends_with(".brep") || low.ends_with(".brp")) {
                        cad_reload = geom::CadModel::load_brep(cand);
                    } else {
                        continue;
                    }
                    if (cad_reload && !cad_reload->empty()) {
                        topo = geom::extract_topology(*cad_reload, 10);
                        topo_ptr = &(*topo);
                        break;
                    }
                } catch (...) {
                    cad_reload.reset();
                    topo.reset();
                    topo_ptr = nullptr;
                }
            }
        }

        // Live BRep for M10 wall free-slide + OCC re-project (optional).
        const geom::CadModel* cad_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            cad_ptr = &(*model.cad);
        } else if (cad_reload && !cad_reload->empty()) {
            cad_ptr = &(*cad_reload);
        }

        auto fill = mesh::varyhedron_fill_surface(
            model.surface, model.bbox_min, model.bbox_max, h, std::max(1, skin_layers), edges,
            feature_band, seeds, band, turn_deg, topo_ptr, cad_ptr, /*wall_smooth_iters=*/4);
        fill_h = fill.h_fine;
        out.mesh.nodes = std::move(fill.mesh.nodes);
        out.mesh.elements.reserve(fill.mesh.tets.size());
        for (const auto& tet : fill.mesh.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        if (out.boundary_quads.empty()) {
            out.boundary_quads = std::move(fill.mesh.boundary_quads);
        }
        // Packing-seed engine (ADR-0023): sharp-only protect; dual deferred; CVT path later.
        // Protect radii sized by min(α h, β lfs) (CDS / advisor Q6).
        // M10: wall free-slide + OCC surface re-project after sharp snap.
        out.mesher_note = std::format(
            "varyhedron packing (sharp-only edge protect + interior bubble seeds + graded "
            "scaffold + sharp snap + wall OCC project; dual deferred; CVT target): "
            "{} tets, edge_seeds={}, r_protect=[{:.3g},{:.3g}], "
            "sharp/smooth/seam={}/{}/{}, vol_seeds={}, pack_relax={}, "
            "pack_fill={:.3g}, h_bulk={:.4g}/h_fine={:.4g} m, edge_Hd={:.3g} m "
            "(rel={:.3g}, /h={:.3g}, e_chord={:.3g}), wall_nodes={}/moved={}/iters={}{}",
            out.mesh.elements.size(), fill.n_edge_seeds, fill.min_protect_radius,
            fill.max_protect_radius, fill.n_sharp_edges, fill.n_smooth_edges, fill.n_seam_edges,
            fill.n_volume_seeds, fill.n_pack_relax_iters, fill.pack_fill_frac, fill.h_coarse,
            fill.h_fine, fill.edge_profile_hausdorff_max, fill.edge_profile_rel,
            fill.edge_hausdorff_over_h, fill.edge_chordal_efficiency_max, fill.n_wall_nodes,
            fill.n_wall_moved, fill.n_wall_iters,
            topo_ptr ? ", geom_source=brep_topology" : ", geom_source=surface_only");
    } else if (mesher == VolumeMesher::kCvtPoly) {
        // G1–G4 product poly path: constrained restricted CVT → clipped cells → VEM.
        if (!mesh::geogram_available()) {
            throw std::runtime_error(
                "cvt_poly mesher requires POLYMESH_WITH_GEOGRAM (Geogram ConvexCell)");
        }
        mesh::ClipBox box;
        box.min = model.bbox_min;
        box.max = model.bbox_max;
        const Eigen::Vector3d ext = (box.max - box.min).cwiseMax(1e-30);
        const double diag = ext.norm();
        // Lattice density from h so N ~ V/h³ matches N_pred sizing contract.
        const int n_side = std::max(
            2, static_cast<int>(std::lround(std::cbrt(ext.prod()) / std::max(h, 1e-12))));

        std::optional<geom::CadTopology> topo;
        const geom::CadTopology* topo_ptr = nullptr;
        const geom::CadModel* cad_ptr = nullptr;
        if (model.cad && !model.cad->empty()) {
            cad_ptr = &(*model.cad);
            try {
                topo = geom::extract_topology(*model.cad, 8);
                topo_ptr = &(*topo);
            } catch (...) {
                topo.reset();
                topo_ptr = nullptr;
            }
        }

        mesh::ConstrainedLloydParams params;
        params.seed.interior_n_side = n_side;
        params.seed.sharp_sample_stride = 1;
        params.seed.sharp_min_sep_frac = 0.25 * h / diag;
        params.lloyd.max_iters = 24;
        params.lloyd.move_tol_rel = 1e-3;
        // Same h field as N_pred crude predictor (uniform campaign h).
        params.lloyd.size_at = [h](const Eigen::Vector3d&) { return h; };
        params.project_final = (cad_ptr != nullptr);
        params.wall_band_frac = 0.12;
        params.sharp_guard_frac = 0.04;

        const auto cvt = mesh::constrained_lloyd_cvt(box, cad_ptr, topo_ptr, params);
        std::vector<Eigen::Vector3d> positions;
        positions.reserve(cvt.sites.size());
        for (const auto& s : cvt.sites) {
            positions.push_back(s.pos);
        }
        const auto exp = mesh::export_clipped_voronoi(box, positions);
        out.mesh = fea::poly_mesh_to_vem(exp.mesh);
        out.mesh.compact_unused_nodes();
        fill_h = h;
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        out.mesher_note = std::format(
            "cvt_poly restricted CVT (G4 clipped Voronoi → kPolyVem): "
            "{} polys, {} nodes, sites={}/fixed={}, lloyd_iters={}, "
            "sum_vol={:.4g}, n_side={}, h={:.4g} m{}",
            out.mesh.elements.size(), out.mesh.nodes.size(), cvt.sites.size(),
            cvt.seed_stats.n_sharp_fixed, cvt.lloyd_stats.n_iters,
            exp.stats.sum_cell_volume, n_side, h,
            topo_ptr ? ", geom_source=brep_topology" : ", geom_source=bbox_only");
    } else {
        auto fill = mesh::tet_fill_surface(model.surface, model.bbox_min, model.bbox_max, h);
        fill_h = fill.h;
        out.mesh.nodes = std::move(fill.nodes);
        out.mesh.elements.reserve(fill.tets.size());
        for (const auto& tet : fill.tets) {
            out.mesh.elements.push_back(
                fea::NodalElement{fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
        }
        out.boundary_quads = std::move(fill.boundary_quads);
        std::vector<std::array<std::uint32_t, 4>> tet_ids;
        tet_ids.reserve(out.mesh.elements.size());
        for (const auto& el : out.mesh.elements) {
            if (el.nodes.size() == 4) {
                tet_ids.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
            }
        }
        const auto q = mesh::summarize_tet4_quality(out.mesh.nodes, tet_ids);
        // Snap residual from boundary nodes (snap already ran inside tet_fill).
        std::vector<std::uint32_t> btmp;
        for (const auto& qf : out.boundary_quads) {
            btmp.insert(btmp.end(), qf.begin(), qf.end());
        }
        std::sort(btmp.begin(), btmp.end());
        btmp.erase(std::unique(btmp.begin(), btmp.end()), btmp.end());
        const auto conf = mesh::surface_conformity(model.surface, out.mesh.nodes, btmp);
        out.mesher_note = std::format(
            "tet grid fill v1 (Cartesian, not Delaunay): {} tet4, {} nodes, h={:.4g} m, "
            "minQ={:.3f}, meanQ={:.3f}, slivers={}, snap max|d|={:.3g} m mean|d|={:.3g} m",
            out.mesh.elements.size(), out.mesh.nodes.size(), fill_h, q.min_aspect,
            q.mean_aspect, q.n_sliver, conf.max_distance, conf.mean_distance);
    }

    // Prefer true element exterior faces for display/region skin so tet/prism
    // previews show element triangles (incl. Kuhn diagonals), not only lattice quads.
    {
        auto exterior = fea::extract_boundary_faces(out.mesh);
        if (!exterior.empty()) {
            out.boundary_quads = std::move(exterior);
        }
    }

    const auto& surf = model.surface;
    std::set<std::uint32_t> boundary_nodes;
    for (const auto& quad : out.boundary_quads) {
        boundary_nodes.insert(quad.begin(), quad.end());
    }
    for (const auto node : boundary_nodes) {
        const auto& pt = out.mesh.nodes[node];
        double best = std::numeric_limits<double>::max();
        int best_region = -1;
        for (std::size_t ti = 0; ti < surf.triangles.size(); ++ti) {
            const auto& tri = surf.triangles[ti];
            const double d = point_triangle_distance(
                pt, surf.vertices[tri[0]], surf.vertices[tri[1]], surf.vertices[tri[2]]);
            if (d < best) {
                best = d;
                best_region = model.triangle_region[ti];
            }
        }
        if (best <= 1.5 * fill_h) {
            out.boundary_node_region[node] = best_region;
        }
    }
    if (std::abs(tendency_plan.tendency) >= 1e-12 || tendency_plan.remapped) {
        out.mesher_note = std::format(
            "{} | element_tendency={:.3g}→{}{}", out.mesher_note, tendency_plan.tendency,
            tendency_plan.label,
            tendency_plan.remapped ? " (remapped)" : "");
    }
    // Graded LEB / packing can leave orphan node slots (no element refs) that
    // inject zero-stiffness free DOFs and singular K — drop them always.
    const std::size_t n_orphans = out.mesh.compact_unused_nodes();
    if (n_orphans > 0) {
        out.mesher_note += std::format(" | compact_orphans={}", n_orphans);
        out.boundary_quads = fea::extract_boundary_faces(out.mesh);
        out.boundary_node_region.clear();
    }
    out.mesh.check_validity();
    return out;
}

VolumeMeshOutput voxel_mesh(const Model& model, double h) {
    return volume_mesh(model, h, VolumeMesher::kTetFill, 2);
}

namespace {
struct JobCancelled : std::runtime_error {
    JobCancelled() : std::runtime_error("cancelled") {}
};
} // namespace

void SolveJob::set_status(const std::string& s) {
    const std::lock_guard lock(status_mutex_);
    status_ = s;
}

void SolveJob::set_progress(const std::string& phase, double phase_frac, int pass,
                            int pass_count) {
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0_)
                        .count();
    const std::lock_guard lock(status_mutex_);
    progress_.phase = phase;
    progress_.phase_frac = std::clamp(phase_frac, 0.0, 1.0);
    progress_.elapsed_ms = ms;
    progress_.pass = pass;
    progress_.pass_count = pass_count;
}

void SolveJob::report(const std::string& phase, double phase_frac,
                      const std::string& status_msg, int pass, int pass_count) {
    const auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0_)
                        .count();
    const std::lock_guard lock(status_mutex_);
    status_ = status_msg;
    progress_.phase = phase;
    progress_.phase_frac = std::clamp(phase_frac, 0.0, 1.0);
    progress_.elapsed_ms = ms;
    progress_.pass = pass;
    progress_.pass_count = pass_count;
}

void SolveJob::publish_live_mesh(const VolumeMeshOutput& vol) {
    // Boundary-focused copy for the viewport. Keep elements so face type colors
    // work; this runs only at mesh/adapt phase boundaries (not mid-fill).
    {
        const std::lock_guard lock(live_mesh_mutex_);
        live_mesh_ = vol;
    }
    live_mesh_gen_.fetch_add(1, std::memory_order_release);
    note_mesh_stats(vol);
}

void SolveJob::note_mesh_stats(const VolumeMeshOutput& vol) {
    const std::lock_guard lock(status_mutex_);
    progress_.n_elems = vol.mesh.elements.size();
    progress_.n_nodes = vol.mesh.nodes.size();
}

fea::SolveOptions SolveJob::solve_options_with_progress(int pass, int pass_count) {
    fea::SolveOptions opt;
    opt.on_progress = [this, pass, pass_count](int iter, int max_iters, double resid) {
        const double frac =
            max_iters > 0 ? std::clamp(static_cast<double>(iter) / static_cast<double>(max_iters),
                                       0.0, 1.0)
                          : 0.0;
        const auto ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0_)
                            .count();
        const std::lock_guard lock(status_mutex_);
        progress_.phase = "solve";
        progress_.phase_frac = frac;
        progress_.elapsed_ms = ms;
        progress_.pass = pass;
        progress_.pass_count = pass_count;
        progress_.cg_iter = iter;
        progress_.cg_resid = resid;
        status_ = std::format("solving… CG {}/{}  resid {:.3g}", iter, max_iters, resid);
    };
    return opt;
}

std::optional<VolumeMeshOutput> SolveJob::poll_live_mesh(std::uint64_t& seen_gen) const {
    const auto gen = live_mesh_gen_.load(std::memory_order_acquire);
    if (gen == 0 || gen == seen_gen) {
        return std::nullopt;
    }
    const std::lock_guard lock(live_mesh_mutex_);
    if (!live_mesh_) {
        return std::nullopt;
    }
    seen_gen = gen;
    return *live_mesh_;
}

void SolveJob::checkpoint() {
    bool announced = false;
    std::string resume_phase;
    while (pause_.load(std::memory_order_relaxed) &&
           !cancel_.load(std::memory_order_relaxed)) {
        {
            const std::lock_guard lock(status_mutex_);
            if (!announced) {
                resume_phase = progress_.phase.empty() ? "solve" : progress_.phase;
                if (status_.rfind("paused", 0) != 0) {
                    status_ = std::format("paused — {}", status_);
                }
                progress_.phase = "paused";
                announced = true;
            }
            progress_.elapsed_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          t0_)
                    .count();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (cancel_.load(std::memory_order_relaxed)) {
        throw JobCancelled{};
    }
    if (announced) {
        const std::lock_guard lock(status_mutex_);
        // Restore phase token so the UI leaves the paused bar.
        if (progress_.phase == "paused") {
            progress_.phase = resume_phase.empty() ? "solve" : resume_phase;
        }
        if (status_.rfind("paused — ", 0) == 0) {
            status_ = status_.substr(std::string("paused — ").size());
        }
    }
}

void SolveJob::reset_control_flags() {
    cancel_.store(false, std::memory_order_relaxed);
    pause_.store(false, std::memory_order_relaxed);
    t0_ = std::chrono::steady_clock::now();
    {
        const std::lock_guard lock(status_mutex_);
        progress_ = JobProgress{};
    }
    {
        const std::lock_guard lock(live_mesh_mutex_);
        live_mesh_.reset();
    }
    live_mesh_gen_.store(0, std::memory_order_relaxed);
}

std::string SolveJob::status_text() const {
    const std::lock_guard lock(status_mutex_);
    return status_;
}

JobProgress SolveJob::progress() const {
    // Recompute wall-clock on every UI poll. `report()` only stamps phase
    // boundaries; mesh/assemble/solve can run for minutes without another
    // report, which made the progress panel look frozen while CPU was busy.
    JobProgress p;
    {
        const std::lock_guard lock(status_mutex_);
        p = progress_;
    }
    const auto st = state_.load(std::memory_order_relaxed);
    if (st == State::kMeshing || st == State::kSolving) {
        p.elapsed_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0_)
                           .count();
    }
    return p;
}

void SolveJob::request_cancel() {
    cancel_.store(true, std::memory_order_relaxed);
    // Wake a paused worker so it can observe cancel.
    pause_.store(false, std::memory_order_relaxed);
}

void SolveJob::request_pause() {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        pause_.store(true, std::memory_order_relaxed);
    }
}

void SolveJob::request_resume() {
    pause_.store(false, std::memory_order_relaxed);
}

void SolveJob::join_worker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SolveJob::clear_failure() {
    if (state_ == State::kFailed || state_ == State::kCancelled) {
        join_worker();
        state_ = State::kIdle;
        reset_control_flags();
        set_status("idle");
    }
}

void SolveJob::start_mesh(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    join_worker();
    reset_control_flags();
    state_ = State::kMeshing;
    report("mesh", 0.0, "meshing…");
    worker_ = std::thread([this, model, setup] {
        try {
            // Global scalar h from D5 only. Do NOT min-sample geometry sizing at
            // sharp corners for uniform meshers — that forced h→h_min on every
            // CAD box and ~8× DOF, which freezes interactive "mesh only".
            // Feature grading is applied as feature_refine (graded skin / bands).
            const auto resolved = resolve_mesh_size(model, setup.mesh_size);
            const double h = resolved.h;
            report("mesh", 0.15, std::format("meshing… ({}, h={:.4g} m)", resolved.note, h));
            checkpoint();
            mesh_only_ = volume_mesh(model, h, setup.mesher, setup.skin_layers,
                                     setup.use_feature_grading, {}, 0.0,
                                     setup.element_tendency);
            publish_live_mesh(mesh_only_);
            checkpoint();
            mesh_only_.mesher_note =
                std::format("{} | {}", resolved.note, mesh_only_.mesher_note);
            report("done", 1.0,
                   std::format("mesh ready — {} elems, {} nodes | {}",
                               mesh_only_.mesh.elements.size(), mesh_only_.mesh.nodes.size(),
                               mesh_only_.mesher_note));
            state_ = State::kMeshDone;
        } catch (const JobCancelled&) {
            report("cancelled", 0.0, "mesh cancelled");
            state_ = State::kCancelled;
        } catch (const std::exception& e) {
            report("done", 0.0, std::format("mesh failed: {}", e.what()));
            state_ = State::kFailed;
        }
    });
}

namespace {
void fill_result_fields(SolveResult& r, const fea::ZzRecovery& zz, const Eigen::VectorXd& u) {
    r.displacement = u;
    r.global_eta = zz.global_eta;
    r.element_eta = zz.element_eta;
    const auto n_nodes = r.volume_mesh.nodes.size();
    r.nodal_eta.assign(n_nodes, 0.0);
    std::vector<int> counts(n_nodes, 0);
    for (std::size_t e = 0; e < r.volume_mesh.elements.size() && e < zz.element_eta.size();
         ++e) {
        for (auto n : r.volume_mesh.elements[e].nodes) {
            r.nodal_eta[n] += zz.element_eta[e];
            ++counts[n];
        }
    }
    r.max_nodal_eta = 0.0;
    for (std::size_t i = 0; i < n_nodes; ++i) {
        if (counts[i] > 0) {
            r.nodal_eta[i] /= static_cast<double>(counts[i]);
        }
        r.max_nodal_eta = std::max(r.max_nodal_eta, r.nodal_eta[i]);
    }
    const auto& stress = zz.nodal_stress;
    r.von_mises.resize(stress.size());
    r.u_magnitude.resize(stress.size());
    r.max_von_mises = 0.0;
    r.max_displacement = 0.0;
    for (std::size_t i = 0; i < stress.size(); ++i) {
        r.von_mises[i] = fea::von_mises(stress[i]);
        r.u_magnitude[i] = u.segment<3>(3 * static_cast<Eigen::Index>(i)).norm();
        r.max_von_mises = std::max(r.max_von_mises, r.von_mises[i]);
        r.max_displacement = std::max(r.max_displacement, r.u_magnitude[i]);
    }
}
} // namespace

void SolveJob::start(const Model& model, const SimSetup& setup) {
    if (state_ == State::kMeshing || state_ == State::kSolving) {
        return;
    }
    join_worker();
    reset_control_flags();
    state_ = State::kMeshing;
    const int pass_count = std::max(0, setup.adapt_passes);
    report("mesh", 0.0, "meshing…", 0, pass_count);
    // Copy inputs by value into the worker.
    worker_ = std::thread([this, model, setup, pass_count] {
        try {
            // Global h from D5 only (same as start_mesh). Feature grading is
            // feature_refine on graded fills — not global h→h_min at corners.
            const auto resolved = resolve_mesh_size(model, setup.mesh_size);
            const double h = resolved.h;
            double h_use = h;
            report("mesh", 0.15,
                   std::format("meshing… ({}, h={:.4g} m)", resolved.note, h), 0, pass_count);
            checkpoint();
            std::vector<Eigen::Vector3d> adapt_seeds;
            double adapt_seed_band = 0.0;
            // D4: Dörfler element indices for optional local LEB before remesh.
            std::vector<std::size_t> adapt_marked;
            auto vol = volume_mesh(model, h_use, setup.mesher, setup.skin_layers,
                                   setup.use_feature_grading, adapt_seeds, adapt_seed_band,
                                   setup.element_tendency);
            publish_live_mesh(vol);
            checkpoint();
            // Keep resolved-h note on mesher_note for solve mesh_note (GUI/CLI).
            vol.mesher_note = std::format("{} | {}", resolved.note, vol.mesher_note);
            report("assemble", 0.0,
                   std::format("assembling… ({} elements, {} nodes)", vol.mesh.elements.size(),
                               vol.mesh.nodes.size()),
                   0, pass_count);
            state_ = State::kSolving;

            fea::Dirichlet bc;
            std::map<int, std::vector<std::uint32_t>> region_nodes;
            Eigen::VectorXd loads;
            const fea::Material material{.youngs_modulus = setup.youngs_modulus,
                                         .poissons_ratio = setup.poissons_ratio};

            auto assign_boundary_regions = [&](double band) {
                vol.boundary_node_region.clear();
                const auto& surf = model.surface;
                for (std::uint32_t node = 0;
                     node < static_cast<std::uint32_t>(vol.mesh.nodes.size()); ++node) {
                    const auto& pt = vol.mesh.nodes[node];
                    double best = std::numeric_limits<double>::max();
                    int best_region = -1;
                    for (std::size_t ti = 0; ti < surf.triangles.size(); ++ti) {
                        const auto& tri = surf.triangles[ti];
                        const double d = point_triangle_distance(pt, surf.vertices[tri[0]],
                                                                 surf.vertices[tri[1]],
                                                                 surf.vertices[tri[2]]);
                        if (d < best) {
                            best = d;
                            best_region = model.triangle_region[ti];
                        }
                    }
                    if (best <= band) {
                        vol.boundary_node_region[node] = best_region;
                    }
                }
            };

            auto apply_bcs = [&]() {
                bc = fea::Dirichlet{};
                region_nodes.clear();
                for (const auto& [node, region] : vol.boundary_node_region) {
                    region_nodes[region].push_back(node);
                }
                for (const int region : setup.fixtures) {
                    if (const auto it = region_nodes.find(region); it != region_nodes.end()) {
                        for (const auto node : it->second) {
                            bc.fix_node(node);
                        }
                    }
                }
                if (bc.dof_values.empty()) {
                    throw fea::FeaError("no fixtures: fix at least one face before solving");
                }
                loads = Eigen::VectorXd::Zero(
                    3 * static_cast<Eigen::Index>(vol.mesh.nodes.size()));
                for (const auto& [region, load] : setup.loads) {
                    const auto it = region_nodes.find(region);
                    if (it == region_nodes.end() || it->second.empty()) {
                        continue;
                    }
                    const Eigen::Vector3d per_node =
                        load.force / static_cast<double>(it->second.size());
                    for (const auto node : it->second) {
                        loads.segment<3>(3 * static_cast<Eigen::Index>(node)) += per_node;
                    }
                }
            };
            apply_bcs();

            auto element_centroids = [&](const fea::NodalMesh& m) {
                std::vector<Eigen::Vector3d> cents;
                cents.reserve(m.elements.size());
                for (const auto& el : m.elements) {
                    Eigen::Vector3d c = Eigen::Vector3d::Zero();
                    for (auto n : el.nodes) {
                        c += m.nodes[n];
                    }
                    cents.push_back(c / static_cast<double>(el.nodes.size()));
                }
                return cents;
            };

            // D3: p-elevate smooth linear elems after last h-pass (or single solve).
            // Explicit flag or auto when adapt_passes > 0 — but never on huge meshes
            // (tet10 ~3–4× DOF; was a common OOM path with graded+feature floods).
            constexpr std::size_t kPElevateMaxNodes = 40000;
            const bool want_p_elevate = setup.p_elevate || setup.adapt_passes > 0;
            auto p_elevate_allowed = [&]() {
                return want_p_elevate && vol.mesh.nodes.size() <= kPElevateMaxNodes;
            };
            const bool do_p_elevate = want_p_elevate; // gate per-call via p_elevate_allowed

            // After mid-edge insertion, assign boundary regions to new nodes that
            // sit between two existing boundary nodes of the same region so
            // fixtures/loads still cover face mid-edge DOFs.
            auto extend_boundary_regions = [&]() {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edge_mid;
                for (const auto& el : vol.mesh.elements) {
                    if (el.type != fea::ElementType::kTet10 &&
                        el.type != fea::ElementType::kHex20) {
                        continue;
                    }
                    const int n_corner = (el.type == fea::ElementType::kTet10) ? 4 : 8;
                    const int n_mid = (el.type == fea::ElementType::kTet10) ? 6 : 12;
                    // Canonical edge order matches p_elevate mid-edge append order.
                    static constexpr std::array<std::array<int, 2>, 6> kTetEdges{
                        {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}}};
                    static constexpr std::array<std::array<int, 2>, 12> kHexEdges{{{0, 1},
                                                                                   {1, 2},
                                                                                   {2, 3},
                                                                                   {3, 0},
                                                                                   {4, 5},
                                                                                   {5, 6},
                                                                                   {6, 7},
                                                                                   {7, 4},
                                                                                   {0, 4},
                                                                                   {1, 5},
                                                                                   {2, 6},
                                                                                   {3, 7}}};
                    for (int m = 0; m < n_mid; ++m) {
                        const auto& epair = (el.type == fea::ElementType::kTet10)
                                                ? kTetEdges[static_cast<std::size_t>(m)]
                                                : kHexEdges[static_cast<std::size_t>(m)];
                        const auto a = el.nodes[static_cast<std::size_t>(epair[0])];
                        const auto b = el.nodes[static_cast<std::size_t>(epair[1])];
                        const auto mid = el.nodes[static_cast<std::size_t>(n_corner + m)];
                        edge_mid[std::minmax(a, b)] = mid;
                    }
                }
                for (const auto& [ab, mid] : edge_mid) {
                    if (vol.boundary_node_region.contains(mid)) {
                        continue;
                    }
                    const auto ita = vol.boundary_node_region.find(ab.first);
                    const auto itb = vol.boundary_node_region.find(ab.second);
                    if (ita != vol.boundary_node_region.end() &&
                        itb != vol.boundary_node_region.end() && ita->second == itb->second) {
                        vol.boundary_node_region[mid] = ita->second;
                    }
                }
            };

            // Joint (h,p,shape) driver policy (ADR-0019 §4). Seed fixed for
            // deterministic product runs; campaign-fitted costs land later.
            // h_min is set once h_adapt_floor is known (below).
            adapt::HpDriverPolicy hp_policy;
            hp_policy.seed = 0x48504452ull; // 'HPDR'
            hp_policy.h_refine_factor = 0.75;

            // Surface geometry attributes for a-priori h gate (once per solve).
            std::vector<double> surf_kappa;
            std::vector<double> surf_thickness;
            if (setup.use_feature_grading && !model.surface.vertices.empty()) {
                try {
                    surf_kappa = geom::estimate_vertex_curvature(model.surface).kappa;
                    surf_thickness =
                        geom::estimate_local_thickness(model.surface).thickness;
                } catch (...) {
                    surf_kappa.clear();
                    surf_thickness.clear();
                }
            }

            auto build_hp_signals = [&](const std::vector<Eigen::Vector3d>& cents,
                                        const std::vector<double>& element_eta) {
                const auto n = element_eta.size();
                std::vector<double> h_loc(n, h_use);
                std::vector<double> kappa(n, 0.0);
                std::vector<double> thick(n, 0.0);
                std::vector<int> p_ord(n, 1);
                for (std::size_t e = 0; e < n && e < vol.mesh.elements.size(); ++e) {
                    const auto& el = vol.mesh.elements[e];
                    if (el.type == fea::ElementType::kTet10 ||
                        el.type == fea::ElementType::kHex20) {
                        p_ord[e] = 2;
                    }
                    if (e < cents.size() && !surf_kappa.empty()) {
                        const auto vi = geom::nearest_vertex_index(model.surface, cents[e]);
                        if (vi < surf_kappa.size()) {
                            kappa[e] = surf_kappa[vi];
                        }
                        if (vi < surf_thickness.size() &&
                            geom::has_finite_thickness(surf_thickness[vi])) {
                            thick[e] = surf_thickness[vi];
                        }
                    }
                }
                // Empty surplus → driver estimates from ZZ ranking.
                return adapt::make_hp_signals(h_loc, kappa, thick, element_eta, {}, p_ord,
                                              {}, {}, {}, hp_policy);
            };

            auto maybe_p_elevate = [&](const std::vector<std::size_t>& elevate_idx,
                                       std::string& note_suffix) {
                if (!do_p_elevate || elevate_idx.empty()) {
                    if (do_p_elevate && elevate_idx.empty()) {
                        note_suffix = " p-elev=0";
                    }
                    return false;
                }
                if (!p_elevate_allowed()) {
                    note_suffix = std::format(" p-elev skipped (nodes {}>{} budget)",
                                              vol.mesh.nodes.size(), kPElevateMaxNodes);
                    return false;
                }
                // Only elevate still-linear zoo elements (driver may re-list p=2).
                std::vector<std::size_t> linear;
                linear.reserve(elevate_idx.size());
                for (auto e : elevate_idx) {
                    if (e >= vol.mesh.elements.size()) {
                        continue;
                    }
                    const auto t = vol.mesh.elements[e].type;
                    if (t == fea::ElementType::kTet4 || t == fea::ElementType::kHex8) {
                        linear.push_back(e);
                    }
                }
                if (linear.empty()) {
                    note_suffix = " p-elev=0";
                    return false;
                }
                const auto before = vol.mesh.nodes.size();
                vol.mesh = fea::p_elevate(vol.mesh, linear);
                extend_boundary_regions();
                apply_bcs();
                const auto counts = fea::count_element_types(vol.mesh);
                note_suffix =
                    std::format(" p-elev={} n+{} (tet10={} hex20={})", linear.size(),
                                vol.mesh.nodes.size() - before, counts.tet10, counts.hex20);
                report("recover", 0.5,
                       std::format("p-elevate… ({} → quadratic via hp-driver)", linear.size()),
                       /*pass=*/0, pass_count);
                return true;
            };

            // Geometry-driven hp hybrid (pre-solve): elevate bulk elements whose
            // centroids sit deep inside, away from the free surface / curved skin.
            // Leaves linear tets near features (high κ / boundary) for cheaper
            // resolution of gradients while bulk gets p=2 economy.
            auto maybe_geo_p_elevate = [&]() {
                if (!do_p_elevate || !setup.use_feature_grading) {
                    return;
                }
                if (!p_elevate_allowed()) {
                    vol.mesher_note =
                        std::format("{} | geo-hp skipped (nodes {}>{} budget)",
                                    vol.mesher_note, vol.mesh.nodes.size(), kPElevateMaxNodes);
                    return;
                }
                if (setup.mesher != VolumeMesher::kGradedTet &&
                    setup.mesher != VolumeMesher::kVaryhedron &&
                    setup.mesher != VolumeMesher::kTetFill &&
                    setup.mesher != VolumeMesher::kHybrid &&
                    setup.mesher != VolumeMesher::kHybridVem) {
                    return;
                }
                const double bulk_band = 1.75 * h_use;
                const auto cents = element_centroids(vol.mesh);
                std::vector<std::size_t> bulk;
                bulk.reserve(cents.size() / 2);
                for (std::size_t e = 0; e < cents.size(); ++e) {
                    const auto& el = vol.mesh.elements[e];
                    if (el.type != fea::ElementType::kTet4 &&
                        el.type != fea::ElementType::kHex8) {
                        continue;
                    }
                    const double d =
                        geom::distance_to_surface_vertices(model.surface, cents[e]);
                    if (d >= bulk_band) {
                        bulk.push_back(e);
                    }
                }
                if (bulk.empty()) {
                    return;
                }
                // Cap: never elevate more than 70% of linear elems in one shot.
                const std::size_t cap = std::max<std::size_t>(1, (cents.size() * 7) / 10);
                if (bulk.size() > cap) {
                    bulk.resize(cap);
                }
                const auto before = vol.mesh.nodes.size();
                vol.mesh = fea::p_elevate(vol.mesh, bulk);
                extend_boundary_regions();
                apply_bcs();
                const auto counts = fea::count_element_types(vol.mesh);
                vol.mesher_note =
                    std::format("{} | geo-hp: p-elev bulk {} (tet10={} n+{})", vol.mesher_note,
                                bulk.size(), counts.tet10, vol.mesh.nodes.size() - before);
                report("recover", 0.3,
                       std::format("geo hp-elevate… ({} bulk → quadratic)", bulk.size()), 0,
                       pass_count);
            };
            maybe_geo_p_elevate();
            checkpoint();

            auto mesh_is_all_tet4 = [](const fea::NodalMesh& m) {
                if (m.elements.empty()) {
                    return false;
                }
                for (const auto& el : m.elements) {
                    if (el.type != fea::ElementType::kTet4 || el.nodes.size() != 4) {
                        return false;
                    }
                }
                return true;
            };

            /// ADR-0016: one Rivara LEB wave on marked tets; returns true if mesh grew.
            auto try_local_leb_once = [&](std::span<const std::size_t> marks) -> bool {
                if (marks.empty() || !mesh_is_all_tet4(vol.mesh)) {
                    return false;
                }
                std::vector<std::array<std::uint32_t, 4>> tets;
                tets.reserve(vol.mesh.elements.size());
                for (const auto& el : vol.mesh.elements) {
                    tets.push_back({el.nodes[0], el.nodes[1], el.nodes[2], el.nodes[3]});
                }
                const std::size_t n0 = tets.size();
                try {
                    mesh::LocalRefineStats st;
                    auto refined =
                        mesh::local_refine_tets(vol.mesh.nodes, std::move(tets), marks, &st);
                    if (refined.tets.size() <= n0) {
                        return false;
                    }
                    vol.mesh.nodes = std::move(refined.nodes);
                    vol.mesh.elements.clear();
                    vol.mesh.elements.reserve(refined.tets.size());
                    for (const auto& tet : refined.tets) {
                        vol.mesh.elements.push_back(fea::NodalElement{
                            fea::ElementType::kTet4, {tet[0], tet[1], tet[2], tet[3]}});
                    }
                    vol.mesh.check_validity();
                    // Lattice quads invalid after midpoints — rebuild true exterior faces.
                    vol.boundary_quads = fea::extract_boundary_faces(vol.mesh);
                    assign_boundary_regions(1.5 * h_use);
                    vol.mesher_note = std::format(
                        "{} | local LEB (ADR-0016): +{} tets, +{} nodes, {} bisections",
                        vol.mesher_note, refined.tets.size() - n0, st.n_new_nodes,
                        st.n_bisections);
                    return true;
                } catch (const std::exception&) {
                    return false;
                }
            };

            /// Multi-wave LEB: re-mark by proximity to adapt seeds between waves
            /// so high-error regions deepen without a full remesh each time.
            auto try_local_leb = [&](std::span<const std::size_t> marks) -> bool {
                const int waves = std::clamp(setup.adapt_leb_waves, 1, 4);
                std::vector<std::size_t> current(marks.begin(), marks.end());
                bool any = false;
                int waves_done = 0;
                for (int w = 0; w < waves; ++w) {
                    if (current.empty()) {
                        break;
                    }
                    if (!try_local_leb_once(current)) {
                        break;
                    }
                    any = true;
                    ++waves_done;
                    if (w + 1 >= waves || adapt_seeds.empty()) {
                        break;
                    }
                    // Next wave: tets whose centroid falls in a seed ball.
                    const auto cents = element_centroids(vol.mesh);
                    const double band =
                        adapt_seed_band > 0.0 ? adapt_seed_band : (1.5 * h_use);
                    const double r2 = band * band;
                    current.clear();
                    current.reserve(cents.size() / 4 + 8);
                    for (std::size_t e = 0; e < cents.size(); ++e) {
                        for (const auto& s : adapt_seeds) {
                            if ((cents[e] - s).squaredNorm() <= r2) {
                                current.push_back(e);
                                break;
                            }
                        }
                    }
                    // Cap so a single pass cannot explode DOF (≤ 30% of mesh).
                    const std::size_t cap =
                        std::max<std::size_t>(8, (cents.size() * 3) / 10); // ≤ 30% of mesh
                    if (current.size() > cap) {
                        current.resize(cap);
                    }
                }
                if (any && waves_done > 1) {
                    vol.mesher_note =
                        std::format("{} | LEB waves={}", vol.mesher_note, waves_done);
                }
                return any;
            };

            // Grid budget floor: graded is always 2:1 (fine lattice ≈ h/2).
            const int grid_sub =
                (setup.mesher == VolumeMesher::kGradedTet ||
                 setup.mesher == VolumeMesher::kVaryhedron)
                    ? 2
                    : 1;
            const double h_grid_floor = mesh::min_h_for_cell_budget(
                model.bbox_min, model.bbox_max, mesh::kDefaultMaxGridCells, grid_sub);
            const double h_adapt_floor = std::max(h * 0.35, h_grid_floor);
            hp_policy.h_min = h_adapt_floor;

            // Prefer mesher matching the last shape vote (mesher-tendency will own the
            // continuous dial; here we only flip discrete product meshers when the
            // driver majority-votes).
            auto mesher_for_tendency = [&](VolumeMesher base,
                                           adapt::ShapeTendency t) -> VolumeMesher {
                switch (t) {
                case adapt::ShapeTendency::kPreferTet:
                    if (base == VolumeMesher::kHybrid || base == VolumeMesher::kHybridVem ||
                        base == VolumeMesher::kHexFill || base == VolumeMesher::kHexVem) {
                        return VolumeMesher::kGradedTet;
                    }
                    break;
                case adapt::ShapeTendency::kPreferPoly:
                    if (base == VolumeMesher::kHybrid) {
                        return VolumeMesher::kHybridVem;
                    }
                    break;
                case adapt::ShapeTendency::kPreferHex:
                    if (base == VolumeMesher::kTetFill || base == VolumeMesher::kGradedTet) {
                        return VolumeMesher::kHybrid;
                    }
                    break;
                case adapt::ShapeTendency::kKeep:
                default:
                    break;
                }
                return base;
            };
            adapt::ShapeTendency last_shape_vote = adapt::ShapeTendency::kKeep;

            for (int pass = 0; pass <= setup.adapt_passes; ++pass) {
                checkpoint();
                // Overall progress across adapt passes (pass 0..N).
                const double pass_base =
                    static_cast<double>(pass) / static_cast<double>(pass_count + 1);
                const double pass_span = 1.0 / static_cast<double>(pass_count + 1);
                if (pass > 0) {
                    // D4: prefer true local LEB on tet meshes when marks exist.
                    const bool tet_path = setup.mesher == VolumeMesher::kTetFill ||
                                          setup.mesher == VolumeMesher::kGradedTet;
                    bool did_local = false;
                    if (tet_path && !adapt_marked.empty()) {
                        did_local = try_local_leb(adapt_marked);
                    }
                    if (!did_local) {
                        // Prefer graded mesher when local seeds are available so
                        // a posteriori balls can refine without global h→0.
                        auto mesher_adapt = (!adapt_seeds.empty() &&
                                             (setup.mesher == VolumeMesher::kTetFill ||
                                              setup.mesher == VolumeMesher::kGradedTet))
                                                ? VolumeMesher::kGradedTet
                                                : setup.mesher;
                        mesher_adapt = mesher_for_tendency(mesher_adapt, last_shape_vote);
                        // Remesh: graded always uses ÷2 lattice budget (fine ≈ h/2).
                        const int remesh_sub =
                            (!adapt_seeds.empty() || mesher_adapt == VolumeMesher::kGradedTet)
                                ? 2
                                : 1;
                        const double h_remesh =
                            std::max(h_use, mesh::min_h_for_cell_budget(
                                                model.bbox_min, model.bbox_max,
                                                mesh::kDefaultMaxGridCells, remesh_sub));
                        report("mesh", pass_base,
                               std::format("adapt remesh {}… ({} seeds)", pass,
                                           adapt_seeds.size()),
                               pass, pass_count);
                        checkpoint();
                        vol = volume_mesh(model, h_remesh, mesher_adapt, setup.skin_layers,
                                          setup.use_feature_grading, adapt_seeds,
                                          adapt_seed_band, setup.element_tendency);
                        publish_live_mesh(vol);
                        h_use = std::max(h_use, h_remesh);
                    } else {
                        publish_live_mesh(vol); // local LEB also changes connectivity
                    }
                    apply_bcs();
                    // Re-apply geometry hp after remesh so bulk stays quadratic.
                    if (!did_local) {
                        maybe_geo_p_elevate();
                    }
                    report("solve", pass_base + 0.15 * pass_span,
                           std::format("adapt pass {}… ({} elems, {} seeds{})", pass,
                                       vol.mesh.elements.size(), adapt_seeds.size(),
                                       did_local ? ", local LEB" : ""),
                           pass, pass_count);
                } else {
                    report("solve", pass_base + 0.2 * pass_span,
                           std::format("solving… ({} elements, {} nodes)",
                                       vol.mesh.elements.size(), vol.mesh.nodes.size()),
                           pass, pass_count);
                }
                checkpoint();
                const auto solve_opt = solve_options_with_progress(pass, pass_count);
                auto u_try =
                    fea::solve_elastostatics(vol.mesh, material, bc, loads, solve_opt);
                report("recover", pass_base + 0.7 * pass_span,
                       std::format("recovering stress… (pass {}/{})", pass, pass_count), pass,
                       pass_count);
                checkpoint();
                auto zz_try = fea::recover_zz(vol.mesh, material, u_try);
                const auto cents = element_centroids(vol.mesh);
                const auto signals = build_hp_signals(cents, zz_try.element_eta);
                const auto hp_plan =
                    adapt::drive_hp(signals, hp_policy, cents, h_use);
                last_shape_vote = hp_plan.global_shape;
                const std::string hp_note = adapt::summarize_hp_plan(hp_plan);

                // D2: global η target — stop when η is small enough (0 = disabled).
                if (setup.eta_target > 0.0 && zz_try.global_eta <= setup.eta_target) {
                    std::string pnote;
                    if (maybe_p_elevate(hp_plan.p_mark, pnote)) {
                        publish_live_mesh(vol);
                        u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads,
                                                        solve_options_with_progress(pass, pass_count));
                        zz_try = fea::recover_zz(vol.mesh, material, u_try);
                    }
                    SolveResult r;
                    r.mesh_note = std::format(
                        "{} | {} | eta-target stop η={:.4g}≤{:.4g} pass={}/{} h={:.4g}{}",
                        vol.mesher_note, hp_note, zz_try.global_eta, setup.eta_target, pass,
                        setup.adapt_passes, h_use, pnote);
                    r.volume_mesh = std::move(vol.mesh);
                    r.boundary_quads = std::move(vol.boundary_quads);
                    fill_result_fields(r, zz_try, u_try);
                    result_ = std::move(r);
                    break;
                }
                if (pass < setup.adapt_passes) {
                    const auto& sug = hp_plan.h_suggestion;
                    // Early stop only when neither h nor p wants work.
                    if (sug.n_marked == 0 && sug.h_next >= h_use * 0.98 &&
                        hp_plan.p_mark.empty()) {
                        std::string pnote;
                        // Still try mark_smooth fallback if driver was silent on p
                        // but residual remains (legacy complement path).
                        auto p_idx = hp_plan.p_mark;
                        if (p_idx.empty()) {
                            p_idx = adapt::mark_smooth(zz_try.element_eta, 0.3);
                        }
                        if (maybe_p_elevate(p_idx, pnote)) {
                            publish_live_mesh(vol);
                            u_try = fea::solve_elastostatics(
                                vol.mesh, material, bc, loads,
                                solve_options_with_progress(pass, pass_count));
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                        }
                        SolveResult r;
                        r.mesh_note = std::format("{} | {} | adapt early-stop h={:.4g}{}",
                                                  vol.mesher_note, hp_note, h_use, pnote);
                        r.volume_mesh = std::move(vol.mesh);
                        r.boundary_quads = std::move(vol.boundary_quads);
                        fill_result_fields(r, zz_try, u_try);
                        result_ = std::move(r);
                        break;
                    }
                    // Mid-loop p-raise when driver prefers p and marks few h cells.
                    if (!hp_plan.p_mark.empty() &&
                        hp_plan.h_mark.size() * 4 < hp_plan.p_mark.size()) {
                        std::string pnote;
                        if (maybe_p_elevate(hp_plan.p_mark, pnote)) {
                            publish_live_mesh(vol);
                            u_try = fea::solve_elastostatics(
                                vol.mesh, material, bc, loads,
                                solve_options_with_progress(pass, pass_count));
                            zz_try = fea::recover_zz(vol.mesh, material, u_try);
                            vol.mesher_note =
                                std::format("{} | mid-loop{}", vol.mesher_note, pnote);
                        }
                    }
                    h_use = std::max(sug.h_next, h_adapt_floor);
                    adapt_seeds = sug.refine_seeds;
                    adapt_seed_band = sug.seed_band;
                    adapt_marked = hp_plan.h_mark.empty()
                                       ? adapt::dorfler_mark(zz_try.element_eta, 0.3)
                                       : hp_plan.h_mark;
                    continue;
                }
                std::string pnote;
                auto p_idx = hp_plan.p_mark;
                if (p_idx.empty() && do_p_elevate) {
                    p_idx = adapt::mark_smooth(zz_try.element_eta, 0.3);
                }
                if (maybe_p_elevate(p_idx, pnote)) {
                    publish_live_mesh(vol);
                    u_try = fea::solve_elastostatics(vol.mesh, material, bc, loads,
                                                    solve_options_with_progress(pass, pass_count));
                    zz_try = fea::recover_zz(vol.mesh, material, u_try);
                }
                SolveResult r;
                r.mesh_note = std::format(
                    "{} | {} | adapt_passes={} h={:.4g} seeds={}{}", vol.mesher_note,
                    hp_note, setup.adapt_passes, h_use, adapt_seeds.size(), pnote);
                r.volume_mesh = std::move(vol.mesh);
                r.boundary_quads = std::move(vol.boundary_quads);
                fill_result_fields(r, zz_try, u_try);
                result_ = std::move(r);
            } // adapt passes
            report("done", 1.0,
                   std::format("done — max von Mises {:.4g} MPa, max deflection {:.4g} mm",
                               result_.max_von_mises / 1e6, result_.max_displacement * 1e3),
                   pass_count, pass_count);
            state_ = State::kDone;
        } catch (const JobCancelled&) {
            report("cancelled", 0.0, "solve cancelled");
            state_ = State::kCancelled;
        } catch (const std::exception& e) {
            report("done", 0.0, std::format("solve failed: {}", e.what()));
            state_ = State::kFailed;
        }
    });
}

std::optional<SolveResult> SolveJob::take_result() {
    if (state_ != State::kDone) {
        return std::nullopt;
    }
    join_worker();
    state_ = State::kIdle;
    return std::move(result_);
}

std::optional<VolumeMeshOutput> SolveJob::take_mesh() {
    if (state_ != State::kMeshDone) {
        return std::nullopt;
    }
    join_worker();
    state_ = State::kIdle;
    return std::move(mesh_only_);
}

SolveJob::~SolveJob() { join_worker(); }

} // namespace polymesh::pipeline
