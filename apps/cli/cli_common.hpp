// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Private to the polymesh CLI: argument parsing shared by the command
// translation units, plus their entry points. BC selection and load assembly
// live in fea/bc_selection.hpp and pipeline::cad_pressure_area.

#include "fea/bc_selection.hpp"
#include "fea/traction.hpp"
#include "pipeline/scene.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace polymesh::cli {

/// Prints the command-line help to stderr; returns exit code 2.
int usage();

/// Parses the non-negative integer after a ceiling flag at args[i] into `value`,
/// advancing i. Returns false when it is missing, negative, malformed or too large.
bool parse_ceiling(std::span<char*> args, std::size_t& i, std::size_t& value);

/// `--scale <factor>`: uniform unit conversion applied by `pipeline::Model::load`
/// before anything is derived, so `-h`, BC boxes, exported coordinates and the
/// diag bbox are in scaled units (solver lengths are metres: `--scale 0.001` for
/// a millimetre part). A non-positive or non-finite factor is an error, never 1.0.
bool parse_scale(std::span<char*> args, std::size_t& i, double& scale);

/// One line, only when the import was rescaled, so a log reader can see that
/// every number that follows is in converted units.
void report_scale(double scale);

/// `--mesher` accepts every spelling in `pipeline::mesher_from_name`. An
/// unrecognised name is an error, never a fallback mesher: a silent substitution
/// would mislabel every log line and JSON field that reports the mesher.
bool parse_mesher_arg(const std::string& m, polymesh::pipeline::VolumeMesher& out);

struct BoxSel {
    bool set = false;
    Eigen::Vector3d lo = Eigen::Vector3d::Zero();
    Eigen::Vector3d hi = Eigen::Vector3d::Zero();

    /// The box as a selection region; nullopt when the flag was not given.
    std::optional<polymesh::fea::LoadRegion> region() const {
        if (!set) {
            return std::nullopt;
        }
        return polymesh::fea::LoadRegion{lo, hi};
    }
};

// Parse the 6 numbers following a --fix-box / --load-box flag at args[i].
// On success advances i past the 6 values and sets `b`; returns false on error.
bool parse_box6(std::span<char*> args, std::size_t& i, BoxSel& b);

// Geometry+BC refine regions from optional fix/load boxes (loads finest).
std::vector<polymesh::pipeline::RefineRegion> make_regions(const BoxSel& fix,
                                                           const BoxSel& load);

// Parse --load-dir / --force / --traction at args[i]; advances i past values.
// Returns false when the flag is unknown or its values are missing/invalid.
// Of --force/--traction, the last one given wins.
bool parse_load_flag(std::span<char*> args, std::size_t& i,
                     polymesh::fea::SurfaceLoadSpec& spec);

int cmd_check(std::span<char*> args);
int cmd_mesh(std::span<char*> args);
int cmd_solve(std::span<char*> args);
int cmd_diag(std::span<char*> args);
int cmd_render(std::span<char*> args);
int cmd_calibrate(std::span<char*> args);
/// `polymesh backend`: prints the compute backend + OpenMP/opt summary.
int cmd_backend();

} // namespace polymesh::cli
