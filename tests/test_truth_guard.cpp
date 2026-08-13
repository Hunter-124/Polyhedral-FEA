// SPDX-License-Identifier: BSD-3-Clause

// bench/reference/corpus/*.json is the truth every campaign is scored against, and
// most of it is now INDEPENDENT of this engine: 64 references come from Gmsh
// meshing the STEP plus CalculiX solving it, 8 are closed-form. That independence
// is the whole value of the corpus.
//
// Two scripts write those files (scripts/advisor/promote_truth.py and
// scripts/gen_primitive_corpus.py). promote_truth.py used to protect a metric only
// when its source was exactly "analytic" -- a denylist keyed on the sources that
// existed when it was written. Every externally sourced metric was therefore one
// command away from being silently replaced by this repo's own overkill-mesher
// value, with its measured tolerance reset to the promoted default. These cases pin
// the inverted rule: an ALLOWLIST of sources we generated ourselves, so anything
// third-party is protected the moment it lands, without editing either script.

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

/// Runs `body` with scripts/truth_guard.py imported as `guard` and
/// scripts/advisor/promote_truth.py as `pt`. A failed assert inside the payload
/// exits non-zero and surfaces its output here.
void run_python(const std::string& name, const std::string& body) {
    const fs::path script = fs::temp_directory_path() / (name + ".py");
    const fs::path out = fs::temp_directory_path() / (name + ".txt");
    {
        std::ofstream stream(script);
        REQUIRE(stream.good());
        stream << "import importlib.util, sys\n"
                  "from pathlib import Path\n"
                  "sys.path.insert(0, str(Path('scripts').resolve()))\n"
                  "def _load(alias, path):\n"
                  "    spec = importlib.util.spec_from_file_location(\n"
                  "        alias, Path(path).resolve())\n"
                  "    mod = importlib.util.module_from_spec(spec)\n"
                  "    spec.loader.exec_module(mod)\n"
                  "    return mod\n"
                  "guard = _load('truth_guard', 'scripts/truth_guard.py')\n"
                  "pt = _load('promote_truth', 'scripts/advisor/promote_truth.py')\n"
               << body;
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

TEST_CASE("truth guard: only self-generated sources are overwritable") {
    // The allowlist itself. `analytic`, every `external-*` chain and any source
    // invented later must be protected without touching the guard.
    run_python("polymesh_guard_allowlist", R"PY(
assert guard.SELF_GENERATED_SOURCES == frozenset({"provisional", "overkill-reference"}), \
    guard.SELF_GENERATED_SOURCES
for ours in ("provisional", "overkill-reference"):
    assert guard.protected_source({"source": ours}) is None, ours
for theirs in ("analytic",
               "external-gmsh-mesh+calculix-solver",
               "external-something-not-invented-yet",
               "third-party-fea-2031",
               "hand-calc"):
    assert guard.protected_source({"source": theirs}) == theirs, theirs
# Missing / empty / non-string source must fail CLOSED, not open.
assert guard.protected_source({}) == "<no source field>"
assert guard.protected_source({"source": ""}) == "<no source field>"
assert guard.protected_source({"source": None}) == "<no source field>"
print("ok")
)PY");
}

TEST_CASE("truth guard: promotion refuses to overwrite an external reference") {
    // THE REGRESSION. A reference sourced from the external chain, with a fresh
    // overkill row measuring a different value, must come back untouched: same
    // value, same measured tol, same source. If this fails, promote_truth.py has
    // gone back to silently replacing independent truth with our own answer.
    run_python("polymesh_guard_refuses_external", R"PY(
from pathlib import Path

reference = {
    "part": "channel_s0_c0",
    "truth_source": "external-gmsh-mesh+calculix-solver",
    "metrics": [{
        "name": "strain_energy",
        "value": 1.1404038139680051e-05,
        "tol": 0.02,
        "probe": {"kind": "strain_energy"},
        "source": "external-gmsh-mesh+calculix-solver",
    }],
}
# An overkill row that measured something materially different.
row = {
    "part": "channel_s0_c0",
    "accuracy": {"all": [{"metric": "strain_energy", "value": 9.9e-05}]},
    "action": {}, "n_dof": 999999,
}
updated, promoted, protected = pt.promote(reference, row, Path("bench/campaigns/x/results.jsonl"))
assert promoted == 0, promoted
assert protected == [("strain_energy", "external-gmsh-mesh+calculix-solver")], protected
assert updated == reference, "external reference was MUTATED"
metric = updated["metrics"][0]
assert metric["value"] == 1.1404038139680051e-05, metric["value"]
assert metric["tol"] == 0.02, metric["tol"]          # measured tol, not PROMOTED_TOL
assert metric["source"] == "external-gmsh-mesh+calculix-solver", metric["source"]
assert updated["truth_source"] == "external-gmsh-mesh+calculix-solver"

# A closed-form reference is equally protected.
analytic = {
    "part": "box_hole_s0_c0",
    "truth_source": "analytic",
    "metrics": [{"name": "scf", "value": 3.091, "tol": 0.02,
                 "probe": {"kind": "peak_vm_over_nominal", "nominal": 1.0e6},
                 "source": "analytic"}],
}
row_scf = {"accuracy": {"all": [{"metric": "scf", "value": 2.5}]}, "action": {}}
updated, promoted, protected = pt.promote(analytic, row_scf, Path("x/results.jsonl"))
assert promoted == 0 and updated == analytic, (promoted, updated)
assert protected == [("scf", "analytic")], protected
print("ok")
)PY");
}

TEST_CASE("truth guard: promotion still updates truth this repo generated") {
    // The guard must not seize up: our own provisional seed and a prior
    // overkill-reference are exactly what promotion exists to replace.
    run_python("polymesh_guard_allows_ours", R"PY(
from pathlib import Path

for ours in ("provisional", "overkill-reference"):
    reference = {
        "part": "channel_s0_c0",
        "truth_source": ours,
        "metrics": [{"name": "strain_energy", "value": 1.0, "tol": 1.0,
                     "probe": {"kind": "strain_energy"}, "source": ours}],
    }
    row = {"accuracy": {"all": [{"metric": "strain_energy", "value": 2.5}]},
           "action": {}, "n_dof": 1000}
    updated, promoted, protected = pt.promote(reference, row, Path("x/results.jsonl"))
    assert promoted == 1, (ours, promoted)
    assert protected == [], (ours, protected)
    metric = updated["metrics"][0]
    assert metric["value"] == 2.5, metric
    assert metric["tol"] == pt.PROMOTED_TOL, metric
    assert metric["source"] == "overkill-reference", metric
    assert updated["truth_source"] == "overkill-reference", updated["truth_source"]
print("ok")
)PY");
}

TEST_CASE("truth guard: the explicit escape hatch is required to overwrite") {
    // The one sanctioned override. It must be OFF by default (proven above) and
    // must actually work when asked, so the guard is a guard and not a wall.
    run_python("polymesh_guard_force", R"PY(
from pathlib import Path

reference = {
    "part": "channel_s0_c0",
    "truth_source": "external-gmsh-mesh+calculix-solver",
    "metrics": [{"name": "strain_energy", "value": 1.0, "tol": 0.02,
                 "probe": {"kind": "strain_energy"},
                 "source": "external-gmsh-mesh+calculix-solver"}],
}
row = {"accuracy": {"all": [{"metric": "strain_energy", "value": 7.5}]},
       "action": {}, "n_dof": 1000}
updated, promoted, protected = pt.promote(
    reference, row, Path("x/results.jsonl"), force_external=True)
assert promoted == 1, promoted
assert protected == [("strain_energy", "external-gmsh-mesh+calculix-solver")], protected
assert updated["metrics"][0]["value"] == 7.5, updated["metrics"][0]
assert updated["metrics"][0]["source"] == "overkill-reference"
print("ok")
)PY");
}

TEST_CASE("truth guard: no externally sourced corpus truth is promotable") {
    // Guards the live artefact, not just the logic. The invariant is NOT "nothing
    // in the corpus is promotable" -- newly seeded families legitimately carry
    // `provisional` metrics, and promoting those is precisely what promote_truth.py
    // exists to do. The invariant that matters is that nothing INDEPENDENTLY
    // sourced is promotable: a commit that lets an analytic or external-* truth be
    // overwritten by our own solve trips here, while corpus growth does not.
    run_python("polymesh_guard_corpus", R"PY(
import json
from pathlib import Path

corpus = sorted(Path("bench/reference/corpus").glob("*.json"))
assert len(corpus) >= 72, len(corpus)
promotable_external = []
sources = {}
for path in corpus:
    reference = json.loads(path.read_text(encoding="utf-8"))
    metrics = reference.get("metrics", [])
    assert metrics, path
    for metric in metrics:
        source = metric.get("source")
        sources[source] = sources.get(source, 0) + 1
        independent = source == "analytic" or (
            isinstance(source, str) and source.startswith("external-"))
        if independent and guard.protected_source(metric) is None:
            promotable_external.append((path.name, metric.get("name"), source))
assert not promotable_external, f"independent truth is promotable: {promotable_external}"

# And every source present is either ours to overwrite or protected -- no third
# category can appear unnoticed.
for source in sources:
    assert source in guard.SELF_GENERATED_SOURCES or guard.protected_source(
        {"source": source}) is not None, source
print("corpus metric sources:", sources)
print("ok")
)PY");
}
