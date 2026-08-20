// SPDX-License-Identifier: BSD-3-Clause
//
// bench/reference/external_truth.py meshes each corpus STEP with Gmsh and
// solves it with CalculiX to produce truth independent of this engine. Its
// fixture selection must match the engine's own rule (ADR-0038): a fix box
// selects the BOUNDARY nodes inside the box, never a volume of nodes — under
// the old volume rule an element wholly inside the slab is strain-free, which
// embeds a rigid inclusion and makes the "external" truth measure a different
// problem. The selection is inline in the harness, so this test pins the
// extracted `fixed_boundary_nodes` against a synthetic block where the box
// provably contains one interior node.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

const char* python_exe() {
#if defined(_WIN32)
    if (std::system("python -c \"import sys\" >nul 2>&1") == 0) {
        return "python";
    }
    return "python3";
#else
    return "python3";
#endif
}

void run_python(const std::string& name, const std::string& body) {
    const fs::path script = fs::temp_directory_path() / (name + ".py");
    const fs::path out = fs::temp_directory_path() / (name + ".txt");
    {
        std::ofstream stream(script);
        REQUIRE(stream.good());
        stream << body;
    }
    // Working directory is the repo root (catch_discover_tests WORKING_DIRECTORY).
    const std::string cmd = std::string(python_exe()) + " \"" + script.string() + "\" > \"" +
                            out.string() + "\" 2>&1";
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::ifstream in(out);
        std::ostringstream text;
        text << in.rdbuf();
        FAIL("python payload failed:\n" << text.str());
    }
}

} // namespace

TEST_CASE("peer harness: a fix box selects boundary nodes, never a volume") {
    run_python("polymesh_peer_harness_fixbox", R"PY(
import importlib.util, sys
from pathlib import Path
import numpy as np

spec = importlib.util.spec_from_file_location(
    "external_truth", Path("bench/reference/external_truth.py").resolve())
et = importlib.util.module_from_spec(spec)
# dataclass introspection needs the module registered before exec_module.
sys.modules["external_truth"] = et
spec.loader.exec_module(et)

# 3x3x3 node block. Node 13 = (1,1,1) is the one interior node; the other 26
# are on the block's surface.
nodes = np.array([(x, y, z) for x in (0, 1, 2) for y in (0, 1, 2) for z in (0, 1, 2)],
                 dtype=float)
def nid(x, y, z):
    return x * 9 + y * 3 + z

# Boundary triangles of the block: two per unit square on each outer plane.
faces = []
for axis, fixed_vals in ((0, (0, 2)), (1, (0, 2)), (2, (0, 2))):
    a, b = (axis + 1) % 3, (axis + 2) % 3
    for fv in fixed_vals:
        for i in (0, 1):
            for j in (0, 1):
                p = [[0, 0, 0] for _ in range(4)]
                for k, (da, db) in enumerate(((0, 0), (1, 0), (1, 1), (0, 1))):
                    p[k][axis] = fv
                    p[k][a] = i + da
                    p[k][b] = j + db
                ids = [nid(*q) for q in p]
                faces.append([ids[0], ids[1], ids[2]])
                faces.append([ids[0], ids[2], ids[3]])
faces = np.array(faces, dtype=np.int64)

# The box covers the whole x <= 1.5 half, so the interior node (1,1,1) is
# inside it. The boundary-only rule must select the 17 surface nodes with
# x in {0, 1} and never node 13.
box = [[-0.5, -0.5, -0.5], [1.5, 2.5, 2.5]]
fixed = et.fixed_boundary_nodes(nodes, faces, box)
assert nid(1, 1, 1) not in fixed, "interior node selected by the fix box"
expected = {nid(x, y, z) for x in (0, 1) for y in (0, 1, 2) for z in (0, 1, 2)
            if (x in (0, 2) or y in (0, 2) or z in (0, 2))}
assert set(fixed.tolist()) == expected, (sorted(fixed.tolist()), sorted(expected))

# And the no-op direction: a box around nothing selects nothing.
empty = et.fixed_boundary_nodes(nodes, faces, [[10.0, 10.0, 10.0], [11.0, 11.0, 11.0]])
assert empty.size == 0, empty
print("ok")
)PY");
}
