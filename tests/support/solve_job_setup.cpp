// SPDX-License-Identifier: BSD-3-Clause
#include "support/solve_job_setup.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace polymesh::test_support {

pipeline::SimSetup cantilever_setup(const pipeline::Model& model, double mesh_size) {
    pipeline::SimSetup setup;
    setup.mesh_size = mesh_size;
    setup.mesher = pipeline::VolumeMesher::kTetFill;
    setup.youngs_modulus = 70e9;
    setup.poissons_ratio = 0.33;
    setup.use_feature_grading = false;
    int fixed = -1, loaded = -1;
    for (std::size_t t = 0; t < model.surface.triangles.size(); ++t) {
        double x = 0;
        for (auto v : model.surface.triangles[t]) {
            x += model.surface.vertices[v][0];
        }
        if (x < 1e-12) {
            fixed = model.triangle_region[t];
        }
        if (x > 0.29) {
            loaded = model.triangle_region[t];
        }
    }
    REQUIRE(fixed >= 0);
    REQUIRE(loaded >= 0);
    setup.fixtures.insert(fixed);
    setup.loads[loaded].force = {0, 0, -100};
    return setup;
}

} // namespace polymesh::test_support
