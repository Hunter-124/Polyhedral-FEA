// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to polymesh_testlab: campaign/case/run models and the functions shared
// between its translation units. Normative schemas: docs/dag/interfaces.md.

#ifdef POLYMESH_WITH_ADVISOR
#include "advisor/advisor.hpp"
#endif
#include "fea/material.hpp"
#include "fea/nodal_mesh.hpp"
#include "fea/solve.hpp"
#include "fea/traction.hpp"
#include "geom/cad_model.hpp"
#include "load_area.hpp"
#include "pipeline/scene.hpp"

#include <nlohmann/json.hpp>

#include <Eigen/Core>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace polymesh::testlab::detail {

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace fea = polymesh::fea;
namespace pipeline = polymesh::pipeline;
namespace mesh = polymesh::mesh;
namespace geom = polymesh::geom;
namespace tlab = polymesh::testlab;

// ── campaign / case / reference models ──────────────────────────────────────

struct TierSpec {
    double h_scale = 1.0;
    double keep_frac = 1.0;
};

struct Campaign {
    std::string name;
    std::string host;
    std::vector<std::string> parts; // case file paths (repo-relative)
    std::vector<TierSpec> tiers;
    json grid; // object of arrays
    double w_accuracy = 0.5;
    double w_solve_ms = 0.25;
    double w_mesh_ms = 0.25;
    bool warehouse = false; // ADR-0022 full experiment warehouse
    bool on_finish_analyze = false;
    /// M14: per-run wall-clock (s). 0 → tier defaults 900 / 900 / 2700.
    double max_run_wall_s = 0.0;
    /// M14: pack-level ceiling (s). 0 = unlimited (do not start new runs past it).
    double max_pack_wall_s = 0.0;
    /// Post-mesh DOF / element ceilings for a run. 0 → the throughput defaults
    /// below. A reference campaign legitimately wants a bigger budget than a
    /// throughput sweep: the whole point of an overkill solve is to be larger
    /// than anything the training grid produces.
    long long max_dof = 0;
    long long max_elems = 0;
};

struct Box3 {
    Eigen::Vector3d lo = Eigen::Vector3d::Constant(-1e300);
    Eigen::Vector3d hi = Eigen::Vector3d::Constant(1e300);
    bool contains(const Eigen::Vector3d& p) const {
        return p[0] >= lo[0] && p[0] <= hi[0] && p[1] >= lo[1] && p[1] <= hi[1] &&
               p[2] >= lo[2] && p[2] <= hi[2];
    }
};

struct BcSpec {
    Box3 box;
    std::array<bool, 3> fix{{true, true, true}};
    std::vector<std::uint32_t> cad_face_ids;
};

struct LoadSpec {
    Box3 box;
    Eigen::Vector3d traction = Eigen::Vector3d::Zero(); // N/m^2
    /// Optional guard (m²): selected face area must match within ±2% (advisor Q7).
    std::optional<double> expected_area;
    /// Keep faces whose unit normal aligns with traction (n·t̂ > min_dot).
    /// Default 0.7 when traction is nonzero; ignored when traction ≈ 0.
    double normal_min_dot = 0.7;
    // Exact trimmed BRep faces represented by this selector, resolved from the
    // CAD model once per run. cad_face_area is the sum of the WHOLE trimmed areas
    // of those faces; it drives the face-replacement fallback, but it is NOT a
    // verification target because the selector box may cover only part of a face.
    std::vector<std::uint32_t> cad_face_ids;
    std::optional<double> cad_face_area;
    /// Area of the loaded region obtained by applying THIS case's own selection
    /// rule (load box + |n·t̂| > normal_min_dot) to the exact CAD tessellation.
    /// Being the continuum limit of the same rule, it is mesh-independent and is
    /// both the rescale target for the traction and the basis for the reported
    /// fidelity deficit. Empty when there is no CAD to measure against.
    std::optional<double> cad_rule_area;
};

struct ProbeSpec {
    // Scoring kinds: mean_vm_over_nominal | tip_deflection | strain_energy | ...
    // Diagnostic-only (not preferred for score): max_von_mises, max_vm_over_nominal
    std::string kind;
    double nominal = 0.0;
    /// Optional spatial select for face-mean stress (hole patch, tip face, …).
    std::optional<Box3> select;
};

struct MetricSpec {
    std::string name;
    double value = 0.0;
    double tol = 0.05;
    ProbeSpec probe;
    std::string derivation;
};

struct PartCase {
    std::string part;
    std::string geometry; // path
    double E = 200e9;
    double nu = 0.3;
    double rho = 7850;
    std::vector<BcSpec> bcs;
    std::vector<LoadSpec> loads;
    std::string reference_path;
    std::vector<MetricSpec> metrics; // filled after loading reference
};

struct Config {
    std::string id;
    json values; // original grid values
    pipeline::VolumeMesher mesher = pipeline::VolumeMesher::kHybrid;
    bool feature_refine = true;
    double curvature_turn_deg = 15.0; // recorded only: product volume_mesh uses a fixed 15°
    bool snap_boundary = true;        // recorded only: product path always snaps
    int order = 1;
    double element_tendency = 0.0;
    /// A-priori geometry+BC grading (ADR-0021): refine toward BC/load boxes and
    /// geometry features before the solve. OFF by default so frozen campaign
    /// baselines are unchanged; a campaign opts in via `"bc_grading": true`.
    bool bc_grading = false;
    /// Spectral sizing (ADR-0034): FFT-denoise CAD-edge curvature sources and
    /// energy-truncate the fused size field. OFF by default so frozen campaign
    /// baselines are unchanged; a campaign opts in via `"spectral_smooth": true`.
    bool spectral_smooth = false;
    int skin_layers = 2;
    int adapt_passes = 0;
    double eta_target = 0.0;
    bool p_elevate = false;
    int adapt_leb_waves = 2;
    bool cost_only = false;
    std::optional<double> h_rel;
};

struct Checkpoint {
    std::string campaign;
    std::string state = "running"; // running | paused | finished
    int tier = 0;
    int completed_runs = 0;
    std::vector<std::string> survivors;
    std::string started_utc;
    std::string updated_utc;
    /// Post-campaign hooks that failed; empty when all ran (or none were asked
    /// for). Recorded beside `state` because `state == "finished"` is what a
    /// consumer polls to conclude success.
    std::vector<std::string> hooks_failed;
};

// ── campaign_config.cpp ─────────────────────────────────────────────────────

std::string utc_now();
Campaign load_campaign(const fs::path& path);
PartCase load_case(const fs::path& path);
std::vector<Config> expand_grid(const json& grid);
Checkpoint load_checkpoint(const fs::path& path);
void write_checkpoint(const fs::path& path, const Checkpoint& cp);

// ── campaign_progress.cpp ───────────────────────────────────────────────────

void write_progress(const fs::path& path, const std::string& phase, double phase_frac,
                    double elapsed_ms, const std::string& cfg_id, const std::string& part,
                    int tier, int cg_iter = -1, double cg_resid = -1.0,
                    std::size_t n_elems = 0, std::size_t n_nodes = 0,
                    const std::vector<std::string>& hooks_failed = {});

/// Background progress.json heartbeats so the GUI "live progress" box moves
/// during long mesh/assemble/solve stretches (interfaces.md §6 ~500 ms).
class ProgressHeartbeat {
  public:
    ProgressHeartbeat(fs::path path, std::string phase, std::string cfg_id, std::string part,
                      int tier, std::chrono::steady_clock::time_point t0);
    ProgressHeartbeat(const ProgressHeartbeat&) = delete;
    ProgressHeartbeat& operator=(const ProgressHeartbeat&) = delete;

    void set_phase(std::string phase, double phase_frac = 0.0);
    void set_frac(double f);
    void set_cg(int iter, double resid);
    void set_mesh_stats(std::size_t n_elems, std::size_t n_nodes);

    ~ProgressHeartbeat();

  private:
    void tick_now();
    void loop();

    fs::path path_;
    std::string phase_;
    std::string cfg_id_;
    std::string part_;
    int tier_ = 0;
    std::chrono::steady_clock::time_point t0_{};
    std::mutex mu_;
    std::atomic<bool> stop_;
    std::atomic<double> phase_frac_{0.0};
    std::atomic<int> cg_iter_{-1};
    std::atomic<double> cg_resid_{-1.0};
    std::atomic<std::size_t> n_elems_{0};
    std::atomic<std::size_t> n_nodes_{0};
    std::thread thr_;
};

/// Lightweight boundary mesh for GUI live viewport (campaign dir).
/// Magic "PMP1": nodes float32 xyz, quads uint32×4. No full element dump.
void write_mesh_preview(const fs::path& path, const pipeline::VolumeMeshOutput& vol);

// ── probe_selection.cpp: BC / load selection, probes, solve health ──────────

struct ResolvedLoadFaces {
    std::vector<fea::SurfaceFace> faces;
    /// Area the traction is applied over. In the exact-CAD fallback this is
    /// REPLACED by the exact CAD area, because the traction is rescaled so the
    /// resultant matches the true face; it is then a statement of intent, not a
    /// measurement, and comparing it against the CAD area is vacuous.
    double reported_area = 0.0;
    /// Area the MESH actually selected, always measured, never substituted. This
    /// is the only value that can verify mesh fidelity against the CAD.
    double mesh_selected_area = 0.0;
    double traction_scale = 1.0;
    bool used_exact_fallback = false;
};

struct ProbeAnswers {
    double sigma_max = 0.0;          // DIAGNOSTIC only: global nodal max VM
    double sigma_face_mean = 0.0;    // PRIMARY stress: area-wtd face-region VM (centroid)
    double sigma_box_max = 0.0;      // box-selected peak element-centroid VM
    double sigma_p99 = 0.0;          // DIAGNOSTIC: p99 element-centroid VM, quality-filtered
    double strain_energy = 0.0;      // 1/2 u^T K u (J)
    double tip_deflection = 0.0;     // face-mean |u| on tip/load faces
    double tip_deflection_max = 0.0; // diagnostic: global max |u|
    double mean_u_component = 0.0;   // signed mean of dominant load-dir component
    double mean_ux = 0.0;
    double mean_uz = 0.0;
    int dominant_load_axis = 2; // 0=x,1=y,2=z
    double reaction_sum_err = 0.0;
    double free_residual_rel = 0.0;
    int n_orphan_nodes = 0;
    int n_bc_dofs = 0;
    int n_load_faces = 0;
    int n_probe_nodes = 0;
    int n_quality_excluded = 0; // elements below quality floor for p99
    double load_face_area = 0.0;
    /// Area the mesh actually selected, never substituted (see ResolvedLoadFaces).
    double mesh_selected_area = 0.0;
    /// |A_mesh - A_expected|/A_expected. EMPTY when no expected area could be
    /// established: never 0.0, which would read as a perfect match.
    std::optional<double> load_area_rel_err;
    tlab::LoadAreaStatus load_area_status = tlab::LoadAreaStatus::kUnverified;
    /// Mesh-INDEPENDENT case-definition cross-check: authored expected_area vs the
    /// CAD-rule area. Kept separate from load_area_rel_err because a disagreement
    /// here is a case or geometry bug, not a mesh-quality one.
    std::optional<double> authored_area_rel_diff;
    bool authored_area_consistent = true;
    bool authored_area_checked = false;
    /// Health contribution. FALSE only when the area was actually verified and is
    /// out of tolerance. Unverified and partial-selection are not passes -- they
    /// are reported as their own status -- but they are not solve failures either,
    /// so they must not silently sink a row that is otherwise healthy.
    bool load_area_ok = true;
};

fea::Dirichlet make_dirichlet(const fea::NodalMesh& mesh, const std::vector<BcSpec>& bcs,
                              const geom::CadModel* cad, double h);
std::vector<fea::SurfaceFace> free_faces_as_surface(const fea::NodalMesh& mesh);
Eigen::Vector3d face_centroid(const fea::NodalMesh& mesh, const fea::SurfaceFace& f);
std::vector<ResolvedLoadFaces> resolve_load_faces(const fea::NodalMesh& mesh,
                                                  const geom::CadModel* cad, double h,
                                                  const std::vector<LoadSpec>& loads);
Eigen::VectorXd make_loads(const fea::NodalMesh& mesh, const std::vector<LoadSpec>& loads,
                           const std::vector<ResolvedLoadFaces>& resolved_loads);
std::uint64_t selected_face_set_hash(const std::vector<ResolvedLoadFaces>& selections);
std::uint64_t selected_node_set_hash(const std::vector<ResolvedLoadFaces>& selections);
std::uint64_t load_vector_hash(const Eigen::VectorXd& loads);
std::uint64_t dirichlet_node_set_hash(const fea::Dirichlet& bc);
ProbeAnswers compute_probes(const fea::NodalMesh& mesh, const fea::Material& mat,
                            const Eigen::VectorXd& u, const std::vector<LoadSpec>& loads,
                            const std::vector<ResolvedLoadFaces>& resolved_loads,
                            const std::vector<MetricSpec>& metrics, const fea::Dirichlet& bc,
                            const Eigen::VectorXd& f);
double evaluate_probe(const ProbeSpec& probe, const ProbeAnswers& a);
PartCase with_exact_cad_selections(const pipeline::Model& model, const PartCase& source);

// ── scorecard.cpp: geometry / quality scoring ───────────────────────────────

/// M11: flag sharp CAD edges that want h_edge = L/3 below tier h_min, and count
/// features shorter than 2·h. Detector only — no OCC defeaturing (ADR-0024 Q10).
struct HminFeatureReport {
    json feature_flags = json::array();
    int n_features_below_h_min = 0;
};

json compute_scorecard_geom(const pipeline::Model& model, const fea::NodalMesh& mesh,
                            double h);
double predict_elem_count(const pipeline::Model& model, double h);
json geom_class_of(const pipeline::Model& model, double h_ref);
HminFeatureReport detect_hmin_features(const pipeline::Model& model, double h);
json quality_of(const pipeline::Model& model, const fea::NodalMesh& mesh, double h);
json geo_fidelity_of(const pipeline::Model& model, const fea::NodalMesh& nodal, double h);

// ── run_one.cpp: single run ─────────────────────────────────────────────────

/// Records what the learned advisor would have chosen for a case, so a
/// campaign can score the policy against the grid it actually ran (ADR-0027).
///
/// The advisor deliberately does NOT override the campaign action: the grid is
/// the experiment. Letting the model pick the config would make every row
/// self-confirming and destroy the comparison the campaign exists to produce.
#ifdef POLYMESH_WITH_ADVISOR
class AdvisorScorer {
  public:
    explicit AdvisorScorer(const fs::path& model_dir) : advisor_(model_dir) {}

    [[nodiscard]] json decision_json(const pipeline::CaseFeatures& features) const {
        return json::parse(polymesh::advisor::to_json(advisor_.recommend(features)));
    }

  private:
    polymesh::advisor::Advisor advisor_;
};
#else
class AdvisorScorer {
  public:
    explicit AdvisorScorer(const fs::path&) {
        throw std::runtime_error("--advisor needs a build with POLYMESH_WITH_ADVISOR=ON");
    }
    [[nodiscard]] json decision_json(const pipeline::CaseFeatures&) const {
        return json::object();
    }
};
#endif

struct RunOutcome {
    json line; // results.jsonl object
    // Moved out so the CALLER writes artifacts AFTER the summary row is
    // appended; present only when a mesh was actually built.
    std::optional<pipeline::VolumeMeshOutput> mesh;
    double accuracy_score = 0.0; // 0..1 mean over metrics
    double mesh_ms = 0.0;
    double solve_ms = 0.0;
    double solve_flops = 0.0;
    double solve_bytes = 0.0;
    long long cg_iters = 0;
    std::uint64_t factor_nnz = 0;
    std::string solve_method;
};

void write_warehouse_run(const fs::path& run_dir, const json& line,
                         const pipeline::VolumeMeshOutput* vol) noexcept;
RunOutcome run_one(const Config& cfg, const PartCase& part, int tier, double h_scale,
                   const fs::path& progress_path, const fs::path& mesh_preview_path,
                   const fs::path& warehouse_run_dir = {}, double max_run_wall_s = 900.0,
                   const AdvisorScorer* advisor = nullptr, std::string_view host = {},
                   long long budget_dof = 0, long long budget_elems = 0);

// ── campaign_runner.cpp: successive halving / resume / CLI commands ─────────

int cmd_pause_status(const fs::path& camp_dir);
int cmd_validate(const fs::path& camp_dir);
int run_campaign(const fs::path& camp_dir, bool resume, const AdvisorScorer* advisor);

} // namespace polymesh::testlab::detail
