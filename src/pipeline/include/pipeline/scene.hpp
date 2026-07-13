// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Headless study pipeline: import geometry, CAD-style face regions,
// fixtures/loads/material/mesh settings, tet mesher, background solve.
// apps/gui is presentation-only and consumes this library.

#include "fea/nodal_mesh.hpp"
#include "fea/solve.hpp"
#include "fea/stress.hpp"
#include "geom/cad_model.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace polymesh::pipeline {

enum class VolumeMesher : int {
    kTetFill = 0,
    kHexFill = 1,
    kHexVem = 2,
    kGradedTet = 3,
    kHexPyramid = 4, // hex core + pyramid skin; FE = all-pyramid expand (ADR-0013)
    kPrismSweep = 5, // Cartesian prism6 wedges along dominant axis (ADR-0015 / C3)
    kHybrid = 6,     // hex bulk + pyramid skin; FE expands hex (ADR-0012 v3)
    kOctahedral = 7, // experimental BCC octahedra → tet4 (ADR-0019)
    kHybridVem = 8,  // hex FE bulk + native poly VEM transitions (ADR-0019)
    kVaryhedron = 9, // variable poly packing (ADR-0021); v1 edge-seed scaffold
    kCvtPoly = 10,   // restricted CVT → clipped Voronoi poly VEM (G1–G4 / M5)
};

/// Continuous element-shape preference dial the campaign/tuner sweeps
/// (`element_tendency` ∈ [-1, +1], clamped). See `resolve_element_tendency`.
///
/// Canonical hybrid-family map (base = kHybrid / kHybridVem, |t| > 0):
///   t ≤ -0.50           → kHexFill     (hex bulk)
///  -0.50 < t ≤ +0.25    → kHybrid      (fan-split product FE transitions)
///  +0.25 < t ≤ +0.75    → kHybridVem   (native-poly VEM transitions)
///   t > +0.75           → kGradedTet   (tet bias)
/// Exact t = 0 preserves the requested base mesher (backward compatible).
struct ElementTendencyPlan {
    VolumeMesher mesher = VolumeMesher::kHybrid;
    int skin_layers = 2;
    /// Clamped tendency used for the plan.
    double tendency = 0.0;
    /// True when the hybrid path should emit unsplit poly VEM transitions.
    bool native_poly_transitions = false;
    /// Short label for mesher_note / campaign logs (e.g. "hex", "hybrid-fan").
    const char* label = "hybrid-fan";
    /// True when mesher/skin differ from the inputs (non-zero tendency applied).
    bool remapped = false;
};

/// Map base mesher + continuous tendency dial → concrete mesher / skin / flags.
/// `tendency` is clamped to [-1, +1]. Zero leaves `base` and `skin_layers` as-is
/// (except `native_poly_transitions` is set from whether base is kHybridVem).
ElementTendencyPlan resolve_element_tendency(VolumeMesher base, double tendency,
                                             int skin_layers = 2);

/// Imported model: BRep-first when the source is STEP/BREP (ADR-0020).
/// `cad` retains the live CadModel for product topology/meshing; `surface`
/// is the derived tessellation for regions, viewport, and legacy hybrid fill.
/// STL loads leave `cad` empty (compare/legacy only).
struct Model {
    geom::TriSurface surface;
    /// Retained BRep when loaded from STEP/BREP; empty for STL.
    std::optional<geom::CadModel> cad;
    std::vector<int> triangle_region; // region id per triangle
    int region_count = 0;
    Eigen::Vector3d bbox_min = Eigen::Vector3d::Zero();
    Eigen::Vector3d bbox_max = Eigen::Vector3d::Ones();
    std::string name;
    /// Original filesystem path when loaded from disk (for BRep re-open).
    std::string source_path;

    static Model load(const std::string& path, double sharp_angle_deg = 30.0);
};

/// A force applied to a region: total force vector in newtons.
struct RegionLoad {
    Eigen::Vector3d force = Eigen::Vector3d::Zero();
};

/// Material, mesh, adapt, and BC settings for a study solve.
/// SI throughout: length m, stress/modulus Pa, force N (via RegionLoad).
struct SimSetup {
    /// Young's modulus \(E\), Pa (default steel-scale 200 GPa).
    double youngs_modulus = 200e9;
    /// Poisson's ratio \(\nu\), dimensionless, in (-1, 0.5).
    double poissons_ratio = 0.3;
    /// Target element size \(h\), metres. **0 = auto** via `resolve_mesh_size`
    /// (bbox extent/diagonal + sharp-edge feature density).
    double mesh_size = 0.0;
    bool use_feature_grading = true; // sharp edges + curvature + thin-wall sizing
    /// Max solve→ZZ→(LEB|seed-remesh) refine passes after the initial mesh.
    /// **0 = single mesh+solve** (no adapt). Prefer ≥1 with `eta_target` for
    /// fully adaptive product runs (stops early when η is small enough).
    int adapt_passes = 0;
    /// Stop adapt when global ZZ relative indicator \(\eta \le\) this value.
    /// Dimensionless (energy-norm style); **0 = disabled** (run all passes).
    double eta_target = 0.0;
    /// p-elevate smooth (non-Dörfler) linear elements to tet10/hex20 after the
    /// last h-adapt pass (or after the single solve when adapt_passes=0).
    /// When false, still auto-enables if adapt_passes > 0 (hp product path).
    bool p_elevate = false;
    /// Extra Rivara LEB waves per adapt pass (seed-ball re-mark, no re-solve).
    /// 1 = one LEB (ADR-0016); 2–3 deepen local h before falling back to remesh.
    int adapt_leb_waves = 2;
    int skin_layers = 2; // graded-tet boundary skin depth (coarse cells)
    VolumeMesher mesher = VolumeMesher::kHybrid;
    /// Element-shape preference dial ∈ [-1, +1] (clamped). 0 = respect
    /// `mesher` as-is. Non-zero remaps hybrid-family (and soft-remaps
    /// hex/tet families) toward hex / fan-split hybrid / native-poly VEM /
    /// graded tet. Campaign grid key: `element_tendency`.
    double element_tendency = 0.0;
    std::set<int> fixtures; // region ids with all DOFs fixed
    std::map<int, RegionLoad> loads;
};

/// Resolved mesh size for product mesh / solve paths (D5).
/// When `requested_h > 0`, returns that value. When `requested_h <= 0` (or
/// SimSetup::mesh_size == 0), chooses h0 from bbox extent and diagonal, then
/// tightens using sharp-edge count and shortest feature length so dense CAD
/// creases get a slightly finer default without exploding DOF.
struct ResolvedMeshSize {
    double h = 0.0; // metres
    bool auto_chosen = false;
    std::size_t n_sharp_edges = 0;
    double min_feature_length = 0.0; // metres; 0 if none
    std::string note;                // e.g. "auto h=0.0417 m (extent/24, n_sharp=12)"
};

/// Single source of truth for default h0 (mesh-only, solve, CLI when -h omitted).
ResolvedMeshSize resolve_mesh_size(const Model& model, double requested_h,
                                   double sharp_angle_deg = 30.0);

/// Solve products, ready for rendering / VTU.
struct SolveResult {
    fea::NodalMesh volume_mesh;
    Eigen::VectorXd displacement;    // 3N
    std::vector<double> von_mises;   // per node, Pa
    std::vector<double> u_magnitude; // per node, m
    /// Nodal average of element ZZ indicators (for error-field display).
    std::vector<double> nodal_eta;
    std::vector<double> element_eta; // raw per-element η
    double max_von_mises = 0.0;
    double max_displacement = 0.0;
    double max_nodal_eta = 0.0;
    double global_eta = 0.0; // ZZ indicator
    // Boundary quads of the voxel mesh (node indices), for rendering.
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    std::string mesh_note; // e.g. element/node counts, mesher version
};

/// Volume mesh from closed surface: tet4 grid fill (P2 v1) with stair-cased
/// boundary quads for region mapping / rendering.
struct VolumeMeshOutput {
    fea::NodalMesh mesh;
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    // For every mesh node on the boundary: the region of the nearest STL
    // triangle (used to map picked regions to constraint/load node sets).
    std::map<std::uint32_t, int> boundary_node_region;
    std::string mesher_note;
};
/// Volume fill of a closed model surface.
/// @param h Coarse target edge length, metres (must be > 0; call resolve_mesh_size first).
/// @param feature_refine When true and mesher is graded, also refine near sharp edges.
/// @param refine_seeds Centroids for a posteriori fine blocks, metres (world coords).
/// @param seed_band Ball radius around each seed for graded fine cells, metres (0 = off).
/// @param element_tendency Shape preference ∈ [-1, +1]; see resolve_element_tendency.
VolumeMeshOutput volume_mesh(const Model& model, double h,
                             VolumeMesher mesher = VolumeMesher::kHybrid, int skin_layers = 2,
                             bool feature_refine = false,
                             std::span<const Eigen::Vector3d> refine_seeds = {},
                             double seed_band = 0.0, double element_tendency = 0.0);

/// @deprecated name kept as alias during transition; calls volume_mesh.
/// @param h Target edge length, metres.
VolumeMeshOutput voxel_mesh(const Model& model, double h);

/// Live solve progress for the GUI (same phase vocabulary as
/// docs/dag/interfaces.md §6 progress.json). Updated under the status mutex;
/// poll from the UI thread alongside `state()` / `status_text()`.
struct JobProgress {
    /// mesh | assemble | solve | recover | done | cancelled
    std::string phase;
    double phase_frac = 0.0; // 0–1 within the current phase
    double elapsed_ms = 0.0;
    int pass = 0;       // adapt pass index (0 = initial)
    int pass_count = 0; // setup.adapt_passes (max extra passes)
    int cg_iter = 0;
    double cg_resid = 0.0;
    std::size_t n_elems = 0;
    std::size_t n_nodes = 0;
};

/// Background mesh / solve pipeline. Poll `state` from the UI thread.
class SolveJob {
  public:
    enum class State {
        kIdle,
        kMeshing,
        kSolving,
        kDone,
        kFailed,
        kMeshDone,
        kCancelled,
    };

    void start(const Model& model, const SimSetup& setup);
    /// Mesh only (for viewport preview). Same worker thread rules as start().
    void start_mesh(const Model& model, const SimSetup& setup);
    /// Joins a finished solve worker and returns the result once ready.
    std::optional<SolveResult> take_result();
    /// Joins a finished mesh-only worker.
    std::optional<VolumeMeshOutput> take_mesh();
    /// Clear kFailed / kCancelled → kIdle so the user can retry.
    void clear_failure();

    /// Cooperative cancel: checked between mesh/adapt/solve phases (not mid-CG).
    /// Safe from the UI thread. Worker finishes with State::kCancelled.
    void request_cancel();
    /// Cooperative pause: worker blocks between phases until resume or cancel.
    /// Does not interrupt a single `solve_elastostatics` / mesh call.
    void request_pause();
    void request_resume();
    bool cancel_requested() const { return cancel_.load(std::memory_order_relaxed); }
    bool pause_requested() const { return pause_.load(std::memory_order_relaxed); }

    State state() const { return state_.load(); }
    std::string status_text() const;
    JobProgress progress() const;

    /// Poll intermediate volume mesh for viewport (updated after mesh / adapt
    /// remesh). Returns a copy when generation advanced past `seen_gen` (then
    /// updates `seen_gen`). Cheap no-op when nothing new. Phase-boundary only —
    /// not a mid-fill stream — so cost is one mesh copy per mesh event.
    std::optional<VolumeMeshOutput> poll_live_mesh(std::uint64_t& seen_gen) const;
    std::uint64_t live_mesh_generation() const {
        return live_mesh_gen_.load(std::memory_order_relaxed);
    }

    ~SolveJob();

  private:
    std::atomic<State> state_{State::kIdle};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> pause_{false};
    std::thread worker_;
    SolveResult result_;
    VolumeMeshOutput mesh_only_;
    std::string error_;
    mutable std::mutex status_mutex_;
    std::string status_;
    JobProgress progress_;
    std::chrono::steady_clock::time_point t0_{};
    /// Intermediate mesh for live viewport (worker writes, UI reads).
    mutable std::mutex live_mesh_mutex_;
    std::optional<VolumeMeshOutput> live_mesh_;
    std::atomic<std::uint64_t> live_mesh_gen_{0};
    void set_status(const std::string& s);
    void set_progress(const std::string& phase, double phase_frac, int pass = 0,
                      int pass_count = 0);
    /// Status line + structured progress (same lock).
    void report(const std::string& phase, double phase_frac, const std::string& status_msg,
                int pass = 0, int pass_count = 0);
    void publish_live_mesh(const VolumeMeshOutput& vol);
    void note_mesh_stats(const VolumeMeshOutput& vol);
    /// Between phases: honour pause (spin-sleep) then throw if cancelled.
    void checkpoint();
    void join_worker();
    void reset_control_flags();
    /// Solve options with CG progress wired into JobProgress (when applicable).
    fea::SolveOptions solve_options_with_progress(int pass, int pass_count);
};

} // namespace polymesh::pipeline
