// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// Verification harness (see BENCHMARKS.md).
//
// This is the ONLY module allowed to read bench/reference/ — reference
// values must never appear in, or be read by, mesh, adapt, or fea
// (engineering rule #1 in CLAUDE.md).

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>

namespace polymesh::bench {

class BenchError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// A reference solution loaded from bench/reference/*.json.
struct ReferenceCase {
    /// Case identifier, e.g. "lame-cylinder".
    std::string name;
    /// Human-readable derivation or citation for every value below.
    std::string citation;
    /// Named reference quantities (e.g. "scf" -> 3.0), SI units per case docs.
    std::map<std::string, double> values;
};

/// Loads one reference case from a JSON file under bench/reference/.
/// Throws BenchError on I/O or parse failure.
ReferenceCase load_reference(const std::filesystem::path& path);

/// Parses reference-case JSON text (exposed for unit testing).
ReferenceCase parse_reference(const std::string& json_text);

} // namespace polymesh::bench
