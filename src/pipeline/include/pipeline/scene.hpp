// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Headless study pipeline: import geometry, CAD-style face regions,
// fixtures/loads/material/mesh settings, tet mesher, background solve.
// apps/gui is presentation-only and consumes this library.

#include "fea/nodal_mesh.hpp"
#include "fea/stress.hpp"
#include "geom/tri_surface.hpp"

#include <Eigen/Core>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace polymesh::pipeline {

enum class VolumeMesher : int {
    kTetFill = 0,
    kHexFill = 1,
    kHexVem = 2,
    kGradedTet = 3,
    kHexPyramid = 4, // hex core + pyramid skin (conforming)
};

/// Imported model: triangle surface segmented into CAD-style "faces"
/// (regions of triangles separated by sharp edges), so a click can select
/// a whole planar/smooth face rather than one triangle.
struct Model {
    geom::TriSurface surface;
    std::vector<int> triangle_region; // region id per triangle
    int region_count = 0;
    Eigen::Vector3d bbox_min = Eigen::Vector3d::Zero();
    Eigen::Vector3d bbox_max = Eigen::Vector3d::Ones();
    std::string name;

    static Model load(const std::string& path, double sharp_angle_deg = 30.0);
};

/// A force applied to a region: total force vector in newtons.
struct RegionLoad {
    Eigen::Vector3d force = Eigen::Vector3d::Zero();
};

struct SimSetup {
    double youngs_modulus = 200e9; // Pa
    double poissons_ratio = 0.3;
    double mesh_size = 0.0;          // m; 0 = auto (bbox/30)
    bool use_feature_grading = true; // refine toward sharp edges
    int adapt_passes = 0;            // extra solve→ZZ→refine mesh loops
    int skin_layers = 2;             // graded-tet boundary skin depth
    VolumeMesher mesher = VolumeMesher::kTetFill;
    std::set<int> fixtures; // region ids with all DOFs fixed
    std::map<int, RegionLoad> loads;
};

/// Solve products, ready for rendering.
struct SolveResult {
    fea::NodalMesh volume_mesh;
    Eigen::VectorXd displacement;    // 3N
    std::vector<double> von_mises;   // per node, Pa
    std::vector<double> u_magnitude; // per node, m
    double max_von_mises = 0.0;
    double max_displacement = 0.0;
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
/// `feature_refine`: when true and mesher is graded, also refine near sharp edges.
VolumeMeshOutput volume_mesh(const Model& model, double h,
                             VolumeMesher mesher = VolumeMesher::kTetFill,
                             int skin_layers = 2, bool feature_refine = false);

/// @deprecated name kept as alias during transition; calls volume_mesh.
VolumeMeshOutput voxel_mesh(const Model& model, double h);

/// Background solve pipeline. Poll `state` from the UI thread.
class SolveJob {
  public:
    enum class State { kIdle, kMeshing, kSolving, kDone, kFailed };

    void start(const Model& model, const SimSetup& setup);
    /// Joins a finished worker and returns the result once ready.
    std::optional<SolveResult> take_result();

    State state() const { return state_.load(); }
    std::string status_text() const;
    ~SolveJob();

  private:
    std::atomic<State> state_{State::kIdle};
    std::thread worker_;
    SolveResult result_;
    std::string error_;
    mutable std::mutex status_mutex_;
    std::string status_;
    void set_status(const std::string& s);
};

} // namespace polymesh::pipeline
