// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "pipeline/scene.hpp"

namespace polymesh::test_support {

/// Cantilever SimSetup for SolveJob tests on a box model of length 0.1 in x:
/// tet fill at `mesh_size`, aluminium, no feature grading, the x=0 face region
/// fixed and a {0, 0, -100} N load on the x=0.1 face region.
pipeline::SimSetup cantilever_setup(const pipeline::Model& model, double mesh_size);

} // namespace polymesh::test_support
