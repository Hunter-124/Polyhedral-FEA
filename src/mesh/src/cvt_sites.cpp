// SPDX-License-Identifier: BSD-3-Clause

#include "mesh/cvt_sites.hpp"

#include <algorithm>
#include <cmath>

namespace polymesh::mesh {
namespace {

bool far_enough(const Eigen::Vector3d& p, double min_sep2,
                const std::vector<CvtSite>& sites) {
    for (const CvtSite& s : sites) {
        if ((s.pos - p).squaredNorm() < min_sep2) {
            return false;
        }
    }
    return true;
}

double dist_to_domain_boundary(const Eigen::Vector3d& p, const ClipBox& box) {
    const double dx = std::min(p.x() - box.min.x(), box.max.x() - p.x());
    const double dy = std::min(p.y() - box.min.y(), box.max.y() - p.y());
    const double dz = std::min(p.z() - box.min.z(), box.max.z() - p.z());
    return std::min({dx, dy, dz});
}

}  // namespace

ConstrainedSiteSeedResult seed_constrained_cvt_sites(
    const ClipBox& domain, const geom::CadTopology* topo,
    const ConstrainedSiteSeedParams& params) {
    ConstrainedSiteSeedResult out;
    const double diag = std::max((domain.max - domain.min).norm(), 1e-30);
    const double min_sep = params.sharp_min_sep_frac * diag;
    const double min_sep2 = min_sep * min_sep;
    const int stride = std::max(params.sharp_sample_stride, 1);

    if (topo) {
        for (const geom::CadEdge& e : topo->edges) {
            if (e.feature == geom::CadEdgeFeature::kSeam) {
                ++out.n_seams_skipped;
                continue;
            }
            if (e.feature == geom::CadEdgeFeature::kSmooth) {
                ++out.n_smooth_skipped;
                continue;
            }
            // kSharp only.
            bool used = false;
            for (std::size_t i = 0; i < e.samples.size(); i += static_cast<std::size_t>(stride)) {
                const Eigen::Vector3d& p = e.samples[i];
                // Keep samples inside / near domain.
                if ((p.array() < domain.min.array() - diag).any() ||
                    (p.array() > domain.max.array() + diag).any()) {
                    continue;
                }
                if (!far_enough(p, min_sep2, out.sites)) {
                    continue;
                }
                CvtSite s;
                s.pos = p;
                s.fixed = true;
                out.sites.push_back(s);
                ++out.n_sharp_fixed;
                used = true;
            }
            if (used) {
                ++out.n_sharp_edges_used;
            }
        }
    }

    // Free interior lattice; skip points too close to fixed sharp sites.
    // interior_n_side ≤ 0 means caller injects free sites itself (e.g. surface-
    // interior cell centres for cvt_poly product path).
    if (params.interior_n_side > 0) {
        auto lattice = seed_lattice_sites(domain, params.interior_n_side);
        for (CvtSite& s : lattice) {
            s.fixed = false;
            if (!far_enough(s.pos, min_sep2, out.sites)) {
                continue;
            }
            out.sites.push_back(s);
            ++out.n_interior_free;
        }
    }
    return out;
}

OccSiteProjectStats project_free_wall_sites(const geom::CadModel& cad,
                                            const geom::CadTopology* topo,
                                            const ClipBox& domain,
                                            std::vector<CvtSite>& sites,
                                            double wall_band,
                                            double sharp_guard) {
    OccSiteProjectStats st;
    if (cad.empty() || wall_band <= 0.0) {
        return st;
    }

    double sum_res = 0.0;
    for (CvtSite& s : sites) {
        if (s.fixed) {
            continue;
        }
        if (dist_to_domain_boundary(s.pos, domain) > wall_band) {
            continue;
        }
        ++st.n_candidates;

        if (topo && sharp_guard > 0.0) {
            if (auto q = geom::closest_edge(*topo, s.pos, /*sharp_only=*/true)) {
                if (q->distance < sharp_guard) {
                    ++st.n_skipped_near_sharp;
                    continue;
                }
            }
        }

        auto proj = geom::project_point_on_surface(cad, s.pos);
        if (!proj) {
            ++st.n_failed;
            continue;
        }
        s.pos = proj->point;
        sum_res += proj->distance;
        st.max_residual = std::max(st.max_residual, proj->distance);
        ++st.n_projected;
    }
    if (st.n_projected > 0) {
        st.mean_residual = sum_res / static_cast<double>(st.n_projected);
    }
    return st;
}

ConstrainedLloydResult constrained_lloyd_cvt(const ClipBox& domain,
                                             const geom::CadModel* cad,
                                             const geom::CadTopology* topo,
                                             const ConstrainedLloydParams& params) {
    ConstrainedLloydResult out;
    out.seed_stats = seed_constrained_cvt_sites(domain, topo, params.seed);
    out.sites = out.seed_stats.sites;

    if (out.sites.empty()) {
        return out;
    }

    const double diag = std::max((domain.max - domain.min).norm(), 1e-30);
    const double wall_band = params.wall_band_frac * diag;
    const double sharp_guard = params.sharp_guard_frac * diag;

    // Run Lloyd; optional mid-iteration OCC project by multi-pass.
    CvtLloydParams lloyd = params.lloyd;
    int passes = 1;
    int iters_per = std::max(lloyd.max_iters, 1);
    if (params.project_each_iter && cad && !cad->empty()) {
        passes = std::max(lloyd.max_iters, 1);
        iters_per = 1;
    }
    lloyd.max_iters = iters_per;

    CvtLloydStats last_stats{};
    for (int pass = 0; pass < passes; ++pass) {
        const auto lr = lloyd_cvt(domain, out.sites, lloyd);
        last_stats = lr.stats;
        last_stats.n_iters = (pass + 1) * iters_per;
        for (std::size_t i = 0; i < out.sites.size() && i < lr.positions.size(); ++i) {
            if (!out.sites[i].fixed) {
                out.sites[i].pos = lr.positions[i];
            }
        }
        if (params.project_each_iter && cad && !cad->empty()) {
            out.project_stats = project_free_wall_sites(
                *cad, topo, domain, out.sites, wall_band, sharp_guard);
        }
        if (lr.stats.converged) {
            last_stats.converged = true;
            break;
        }
    }
    out.lloyd_stats = last_stats;

    if (params.project_final && cad && !cad->empty()) {
        out.project_stats = project_free_wall_sites(*cad, topo, domain, out.sites,
                                                    wall_band, sharp_guard);
    }
    return out;
}

}  // namespace polymesh::mesh
