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
#include "mesh/mixed_fill.hpp"
#include "mesh/surface_project.hpp"

#include <Eigen/Core>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>

#include <string>
#include <thread>
#include <vector>
#include <utility>

namespace polymesh::pipeline {
/// Product defaults derive from the hybrid fill's established 48k work budget.
/// Twelve such units sit 25% above the measured legitimate maximum (468,924
/// elements on the public plate+hole auto solve); three displacement DOFs per
/// element is the conservative DOF proxy. CLI/config value 0 selects these.
inline constexpr std::size_t kDefaultMaxMeshElems = 12 * mesh::kHybridMaxElems;
inline constexpr std::size_t kDefaultMaxMeshDof = 3 * kDefaultMaxMeshElems;


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

/// A fully resolved boundary-condition set for one concrete mesh.
/// `loads` is the assembled 3N nodal load vector, not a per-region resultant.
struct BoundaryConditions {
    fea::Dirichlet dirichlet;
    Eigen::VectorXd loads;
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
    /// Maximum solve footprint in decimal GB. 0 = automatic safety cap
    /// (70% of currently available system memory).
    double max_mem_gb = 0.0;
    /// Hard pre-flight ceilings. 0 selects kDefaultMaxMeshElems / Dof.
    /// Set higher explicitly for deliberate large runs.
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
    bool use_feature_grading = true; // sharp edges + curvature + thin-wall sizing
    /// A-priori grade the mesh toward boundary conditions: refine near loaded
    /// and fixed faces (loads finest) before any solve. Complements
    /// `use_feature_grading` (geometry). Off by default to preserve baselines.
    bool bc_grading = false;
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

    /// Optional caller-supplied boundary conditions, evaluated per mesh.
    ///
    /// Empty (the default) keeps the region-based `fixtures`/`loads` selection
    /// above, so the GUI and every existing caller are byte-identical.
    ///
    /// When set, the callback fully replaces that selection and is the sole
    /// source of the Dirichlet set and the load vector. It is re-invoked after
    /// every remesh, LEB pass, and p-elevation, because the mesh it must
    /// select on changes each adapt pass — which is exactly why this is a
    /// callback rather than a precomputed pair.
    ///
    /// Rationale: region selection fixes a whole tessellation region when any
    /// of its triangle centroids falls in a BC box, and spreads a region
    /// resultant over that whole region. A campaign that scores CAD-aware,
    /// mesh-resolved BCs was therefore adapting against a different problem
    /// than it reported. This hook lets such a caller adapt and score the
    /// same system, and reuse `SolveResult::displacement` instead of solving
    /// twice.
    ///
    /// The callback must return a non-empty Dirichlet set, a non-zero load
    /// vector, and `loads.size() == 3 * mesh.nodes.size()`; otherwise the
    /// solve fails with `fea::FeaError`. It must never silently hand back an
    /// unconstrained or unloaded system. Throwing from the callback is
    /// allowed and propagates as a solve failure.
    std::function<BoundaryConditions(const fea::NodalMesh&)> boundary_builder;
};

/// Same pre-flight estimator used by mesh, solve, and diagnostics:
/// \(N_{pred} \approx 6 V_{bbox}/h^3\).
double predict_mesh_elements(const Model& model, double h);
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
    double predicted_elements = 0.0;
    std::size_t element_ceiling = kDefaultMaxMeshElems;
    std::size_t dof_ceiling = kDefaultMaxMeshDof;
    bool ceiling_clamped = false;
};

/// Single source of truth for default h0 (mesh-only, solve, CLI when -h omitted).
ResolvedMeshSize resolve_mesh_size(const Model& model, double requested_h,
                                   double sharp_angle_deg = 30.0,
                                   std::size_t max_elems = 0, std::size_t max_dof = 0);

/// A boundary-condition / load selection region (world AABB) used to grade the
/// mesh toward the **simulation setup**, not just the geometry (ADR-0021).
/// `target_fraction` is the desired edge length inside the box as a fraction of
/// the coarse h: loads finest (e.g. 0.25, stress concentrates under load),
/// fixtures moderate (e.g. 0.5). A box left at ±inf selects the whole part.
struct RefineRegion {
    Eigen::Vector3d lo = Eigen::Vector3d::Constant(-1e300);
    Eigen::Vector3d hi = Eigen::Vector3d::Constant(1e300);
    double target_fraction = 0.5;
};
/// Scale-free geometry and boundary-condition context for learned mesh advice.
/// All geometric measures are normalized by the model bbox diagonal: lengths
/// by diag, areas by diag², volumes by diag³, and curvatures multiplied by diag.
/// Consequently `diag` is 1 for a non-degenerate model (0 otherwise). Counts
/// and Poisson's ratio are dimensionless. Invalid/empty regions contribute zero.
struct CaseFeatures {
    double bbox_dx = 0.0;
    double bbox_dy = 0.0;
    double bbox_dz = 0.0;
    double diag = 0.0;
    double volume = 0.0;
    double surface_area = 0.0;
    double sa_over_v23 = 0.0;
    std::size_t n_faces = 0;
    std::size_t n_sharp_edges = 0;
    double sharp_edge_len_total = 0.0;
    double curved_frac = 0.0;
    double kappa_max_h = 0.0;
    double kappa_mean_h = 0.0;
    double thin_min_over_diag = 0.0;
    double thin_p10_over_diag = 0.0;
    double min_feature_h = 0.0;
    std::size_t n_fix_faces = 0;
    std::size_t n_load_faces = 0;
    double fix_area_frac = 0.0;
    double load_area_frac = 0.0;
    double load_dir_x = 0.0;
    double load_dir_y = 0.0;
    double load_dir_z = 0.0;
    double fix_load_dist_over_diag = 0.0;
    double load_axis_alignment = 0.0;
    double poisson = 0.0;

    // --- exact-BRep descriptors, for the advisor's OOD test only -------------
    //
    // Everything above is measured from the tessellation. These are read from
    // the BRep, and they exist because the mesh proxies cannot answer "is this
    // part unlike anything I was trained on": `curved_frac` above saturates to
    // ~1.0 for any real triangulation.
    //
    // They are NOT network inputs. The shipped ONNX contract is 43 columns and
    // these are not among them; they feed only the Mahalanobis distance in
    // `bench/advisor/ood.json`. `geo_available` is false when the build has no
    // OpenCASCADE or the model carries no BRep, and the advisor must then
    // decline to run the OOD test rather than testing imputed values.
    bool geo_available = false;
    double geo_curved_area_frac = 0.0;
    double geo_cyl_area_frac = 0.0;
    double geo_plane_area_frac = 0.0;
    double geo_other_area_frac = 0.0;
    double geo_min_curv_radius_rel = 0.0;
    double geo_log_curv_radius_mean = 0.0;
    double geo_log_curv_radius_std = 0.0;
    double geo_n_faces = 0.0;
    double geo_n_edges = 0.0;
    double geo_face_area_cv = 0.0;
    double geo_aspect_max = 0.0;
    double geo_aspect_mid = 0.0;
    double geo_volume_frac = 0.0;
    double geo_area_over_v23 = 0.0;
    double geo_min_face_size_rel = 0.0;
};

/// Extract cheap, deterministic advisor context without meshing or solving.
CaseFeatures extract_case_features(const Model& model,
                                  std::span<const RefineRegion> fix_regions,
                                  std::span<const RefineRegion> load_regions,
                                  const Eigen::Vector3d& load_dir, double poisson);

/// Fused geometry + boundary-condition refinement plan for `volume_mesh`.
/// Geometry sources (curvature / thin-wall, a priori) and BC/load region
/// sources (the simulation setup, a priori) are combined into one
/// gradient-limited size field. Legacy refine seeds remain available so
/// a-posteriori ball refinement keeps its existing contract.
struct RefinementPlan {
    mesh::SizeFieldFn size_field;
    std::vector<Eigen::Vector3d> refine_seeds;
    double seed_band = 0.0; // ball influence radius, metres
    double h_min = 0.0;     // finest field target, metres
    double h_fine = 0.0;    // legacy alias for finest requested target, metres
    std::size_t n_geometry_seeds = 0;
    std::size_t n_bc_seeds = 0;
};

/// Build a fused geometry + BC size field for the graded tet and hybrid
/// meshers. `h_coarse` is the resolved bulk size (see resolve_mesh_size).
/// When `use_geometry`, surface curvature / thin-wall sources finer than
/// `h_coarse` are added. Each region contributes surface-face centroids at
/// `target_fraction * h_coarse`. Empty sources produce an empty field/plan.
RefinementPlan build_refinement_plan(const Model& model, double h_coarse,
                                     std::span<const RefineRegion> regions,
                                     bool use_geometry = true);
/// Geometry-volume policy bands. Fill-stage errors above the hard limit never
/// reach a solver; solved-stage errors at or above the truth limit remain
/// useful advisor outcomes but may not define corpus truth.
inline constexpr double kGeometryVolumeTruthLimit = 0.01;
inline constexpr double kGeometryVolumeHardLimit = 0.10;
/// Exact CAD faces farther than this requested-edge fraction from the delivered
/// boundary are unresolved. This is below the measured missing-bore distance;
/// present one-level holes are accepted by the topology test below.
inline constexpr double kGeometryFeatureResolutionOverH = 0.30;

struct GeometryVolumeAssessment {
    bool available = false;
    double mesh_volume = 0.0;
    double cad_volume = 0.0;
    double relative_error = 0.0;
};

class GeometryVolumeLimitError : public std::runtime_error {
  public:
    GeometryVolumeLimitError(std::string message, GeometryVolumeAssessment assessment,
                             bool solved_stage)
        : std::runtime_error(std::move(message)), assessment(assessment),
          solved_stage(solved_stage) {}

    GeometryVolumeAssessment assessment;
    bool solved_stage = false;
};

/// Integrate the physical volume of the actual isoparametric FE/VEM mesh and
/// compare it with the exact CAD solid volume. Quadratic boundary mids therefore
/// contribute to the measured geometry rather than being reduced to corner-only
/// planar faces.
GeometryVolumeAssessment measure_geometry_volume(const Model& model,
                                                 const fea::NodalMesh& mesh);


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
    GeometryVolumeAssessment fill_geometry_volume;
    GeometryVolumeAssessment solved_geometry_volume;

};

/// Build the exact, owner-stable BRep projection oracle used by volume meshing
/// and later p-elevation. The CadModel must outlive `ctx`.
/// Returns false for an empty CAD model or null output storage.
bool make_boundary_projection(const geom::CadModel& cad, double h,
                              mesh::BoundaryProjectionContext* ctx,
                              std::vector<mesh::BoundarySupport>* provenance);

/// Project quadratic free-surface mid-edge nodes onto their exact BRep support.
/// A full move that would invalidate an incident tet10/hex20 is backed off by
/// six bisection steps; only a move with no valid positive fraction is reverted.
/// Returns the number projected to the 0.02h fidelity band. Optional outputs
/// identify true reverts and backed-off nodes that remain outside that band.
std::size_t project_quadratic_boundary_mids(
    fea::NodalMesh& mesh, const geom::CadModel& cad,
    mesh::BoundaryProjectionContext* projection, double h,
    std::vector<std::uint32_t>* reverted_nodes = nullptr,
    std::vector<std::uint32_t>* partial_nodes = nullptr);

/// Volume mesh from closed surface: tet4 grid fill (P2 v1) with stair-cased
/// boundary quads for region mapping / rendering.
struct VolumeMeshOutput {
    fea::NodalMesh mesh;
    std::vector<std::array<std::uint32_t, 4>> boundary_quads;
    /// Boundary subset introduced only by local h/2 solid/void classification.
    std::vector<std::array<std::uint32_t, 4>> local_child_boundary_quads;
    // For every mesh node on the boundary: the region of the nearest STL
    // triangle (used to map picked regions to constraint/load node sets).
    std::map<std::uint32_t, int> boundary_node_region;
    std::string mesher_note;
    GeometryVolumeAssessment fill_geometry_volume;
    GeometryVolumeAssessment solved_geometry_volume;

};
/// Volume fill of a closed model surface.
/// @param h Coarse target edge length, metres (must be > 0; call resolve_mesh_size first).
/// @param feature_refine When true and mesher is graded, also refine near sharp edges.
/// @param refine_seeds Centroids for a posteriori fine blocks, metres (world coords).
/// @param seed_band Ball radius around each seed for graded fine cells, metres (0 = off).
/// @param size_field Optional desired edge length h(x), metres. Forwarded only
/// to the graded tet and hybrid meshers; empty preserves legacy output.
/// @param element_tendency Shape preference ∈ [-1, +1]; see resolve_element_tendency.
/// @param max_elems/max_dof Hard ceilings; 0 disables the low-level guard.
/// Product callers pass the resolved nonzero SimSetup ceilings.
/// @param cancel_check Optional cooperative poll; may throw to cancel meshing.
/// @param auto_retry_budget Bounded coarsen-and-retry count for auto-h callers.
VolumeMeshOutput volume_mesh(
    const Model& model, double h, VolumeMesher mesher = VolumeMesher::kHybrid,
    int skin_layers = 2, bool feature_refine = false,
    std::span<const Eigen::Vector3d> refine_seeds = {}, double seed_band = 0.0,
    double element_tendency = 0.0, std::size_t max_elems = 0, std::size_t max_dof = 0,
    int auto_retry_budget = 0, const std::function<void()>& cancel_check = {},
    const mesh::SizeFieldFn& size_field = {});

/// Refresh the solved-stage assessment after the final order elevation and CAD
/// mid-node projection. Replaces its prior mesher-note token and enforces the
/// same egregious-error hard limit used at fill time.
void update_solved_geometry_volume(const Model& model, VolumeMeshOutput& output);

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
/// One completed solve/recovery pass from the adaptive pipeline.
/// Indicators and shape/cost predictions are dimensionless; timings are ms.
struct PassTrace {
    int pass = 0;
    std::size_t n_elems = 0;
    std::size_t n_nodes = 0;
    std::size_t n_dof = 0;
    double global_eta = 0.0;
    double eta_p50 = 0.0;
    double eta_p90 = 0.0;
    double eta_max = 0.0;
    std::size_t n_h_mark = 0;
    std::size_t n_p_mark = 0;
    std::size_t n_shape_mark = 0;
    int global_shape = 0;
    double predicted_dof_factor = 1.0;
    double mesh_ms = 0.0;
    double solve_ms = 0.0;
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

    /// Cooperative cancel/pause: checked between phases and every few
    /// uninterrupted CG iterations. Safe from the UI thread.
    void request_cancel();
    void request_pause();
    void request_resume();
    bool cancel_requested() const { return cancel_.load(std::memory_order_relaxed); }
    bool pause_requested() const { return pause_.load(std::memory_order_relaxed); }

    State state() const { return state_.load(); }
    std::string status_text() const;
    JobProgress progress() const;
    /// Optional worker-thread callback after each recovered adaptive pass.
    /// Set before `start`; callbacks are serialized in deterministic pass order.
    std::function<void(const PassTrace&)> on_pass;

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
    /// Copied from SimSetup before the worker starts; read only by that worker.
    double active_max_mem_gb_ = 0.0;
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
