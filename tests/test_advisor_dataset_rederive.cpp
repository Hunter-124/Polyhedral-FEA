// SPDX-License-Identifier: BSD-3-Clause

// scripts/build_advisor_dataset.py re-derives each campaign row's accuracy from
// the row's raw `answers` block against the CURRENT bench/reference truth, so
// replacing truth is a seconds-long dataset rebuild instead of an hours-long
// campaign re-run. That makes the Python a mirror of testlab's own accuracy
// computation (apps/testlab/main.cpp: evaluate_probe + the accuracy loop), and a
// silent divergence between the two would corrupt every trained model.
//
// These cases pin the semantics that carry real numeric consequence: which
// ProbeAnswers field each probe kind reads, the rel_err/tol/score formulas, the
// health gate, and — most importantly — that a probe input which was never
// recorded is reported as unscoreable rather than quietly substituted with a
// neighbouring field.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

// Prefer a real `python` on Windows (WindowsApps python3 may be a stub).
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

/// Runs `body` with build_advisor_dataset importable as `bad`. Non-zero exit (an
/// assert inside the payload) fails the test and the payload output is surfaced.
void run_python(const std::string& name, const std::string& body) {
    const fs::path script = fs::temp_directory_path() / (name + ".py");
    const fs::path out = fs::temp_directory_path() / (name + ".txt");
    {
        std::ofstream stream(script);
        REQUIRE(stream.good());
        stream << "import importlib.util, sys\n"
                  "from pathlib import Path\n"
                  "spec = importlib.util.spec_from_file_location(\n"
                  "    'bad', Path('scripts/build_advisor_dataset.py').resolve())\n"
                  "bad = importlib.util.module_from_spec(spec)\n"
                  "spec.loader.exec_module(bad)\n"
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

TEST_CASE("advisor re-derive: SCF probe reads the box-windowed peak, not the global max") {
    // peak_vm_over_nominal is sigma_box_max / nominal. Reading sigma_max instead
    // would still produce a plausible number on every row and silently score the
    // wrong quantity, so pin the field AND the normalisation.
    run_python("polymesh_rederive_scf", R"PY(
answers = {"sigma_box_max": 2.5e6, "sigma_max": 9.9e6, "sigma_face_mean": 1.0e6}
probe = {"kind": "peak_vm_over_nominal", "nominal": 1.0e6}
value, reason = bad.probe_measured(probe, answers)
assert reason == "", reason
assert value == 2.5, value          # 2.5e6 / 1e6, NOT 9.9
value, reason = bad.probe_measured({"kind": "peak_vm"}, answers)
assert value == 2.5e6 and reason == "", (value, reason)
# The mean-stress family must stay on sigma_face_mean.
value, _ = bad.probe_measured({"kind": "scf", "nominal": 1.0e6}, answers)
assert value == 1.0, value
value, _ = bad.probe_measured({"kind": "max_vm_over_nominal", "nominal": 1.0e6}, answers)
assert value == 9.9, value
print("ok")
)PY");
}

TEST_CASE("advisor re-derive: an unrecorded probe input is unscoreable, never substituted") {
    // Rows written before testlab recorded sigma_box_max cannot be re-scored. The
    // only acceptable outcome is a named refusal: falling back to another field
    // would fabricate truth for the whole box_hole family.
    run_python("polymesh_rederive_missing", R"PY(
answers = {"sigma_max": 9.9e6, "sigma_face_mean": 1.0e6}   # no sigma_box_max
value, reason = bad.probe_measured({"kind": "peak_vm_over_nominal", "nominal": 1.0e6}, answers)
assert value is None, value
assert reason == "answers.sigma_box_max_absent", reason

row = {"answers": answers, "health": {"ok": True}}
metrics = [{"name": "scf", "value": 3.0, "tol": 0.1,
            "probe": {"kind": "peak_vm_over_nominal", "nominal": 1.0e6}}]
accuracy, reason = bad.rederive_accuracy(row, metrics)
assert accuracy is None, accuracy
assert reason == "answers.sigma_box_max_absent", reason

# A row that never solved has no answers at all.
accuracy, reason = bad.rederive_accuracy({"health": {"ok": True}}, metrics)
assert accuracy is None and reason == "no_answers", reason
# Without health.ok we cannot reproduce the trust decision, so we refuse.
accuracy, reason = bad.rederive_accuracy({"answers": {"strain_energy": 1.0}}, metrics)
assert accuracy is None and reason == "no_health_ok", reason
print("ok")
)PY");
}

TEST_CASE("advisor re-derive: rel_err, tol floor and the health gate match testlab") {
    run_python("polymesh_rederive_scores", R"PY(
def acc(answers, metrics, ok=True):
    out, reason = bad.rederive_accuracy(
        {"answers": answers, "health": {"ok": ok}}, metrics)
    assert reason == "", reason
    return out

energy = {"kind": "strain_energy"}
# rel_err = |measured - truth| / |truth|; score = 1 / (1 + rel/tol).
out = acc({"strain_energy": 1.1}, [{"name": "e", "value": 1.0, "tol": 0.1, "probe": energy}])
assert abs(out["rel_err"] - 0.1) < 1e-12, out["rel_err"]
assert abs(out["score"] - 0.5) < 1e-12, out["score"]
assert out["trusted"] is True and out["metric"] == "e"

# truth == 0 degrades to the absolute measured value (no division by zero).
out = acc({"strain_energy": 0.25}, [{"name": "e", "value": 0.0, "tol": 0.1, "probe": energy}])
assert out["rel_err"] == 0.25, out["rel_err"]

# tol <= 0 is floored at 1e-12, which collapses the score instead of dividing by zero.
out = acc({"strain_energy": 1.1}, [{"name": "e", "value": 1.0, "tol": 0.0, "probe": energy}])
assert out["score"] < 1e-10, out["score"]

# Health gate: rel_err is still measured, but every score is zeroed and the
# metric is not trusted, so ranking never believes a residual-broken solve.
out = acc({"strain_energy": 1.1},
          [{"name": "e", "value": 1.0, "tol": 0.1, "probe": energy}], ok=False)
assert abs(out["rel_err"] - 0.1) < 1e-12, out["rel_err"]
assert out["score"] == 0.0 and out["trusted"] is False, out

# Multi-metric: accuracy is the FIRST metric plus the full all[] list.
metrics = [{"name": "e", "value": 1.0, "tol": 0.1, "probe": energy},
           {"name": "tip", "value": 2.0, "tol": 0.1, "probe": {"kind": "tip_deflection"}}]
out = acc({"strain_energy": 1.0, "tip_deflection": 2.0}, metrics)
assert out["metric"] == "e" and len(out["all"]) == 2, out
assert [m["metric"] for m in out["all"]] == ["e", "tip"]
assert out["rel_err"] == 0.0 and out["all"][1]["rel_err"] == 0.0

# No metrics -> testlab's explicit "none" shape, with no score/trusted/all keys.
out = acc({"strain_energy": 1.0}, [])
assert out == {"metric": "none", "value": None, "truth": None, "rel_err": None}, out
print("ok")
)PY");
}

TEST_CASE("advisor re-derive: a malformed reference fails loudly") {
    // These are authoring errors in bench/reference, not missing measurements.
    // testlab throws on both; staying silent here would drop metrics from the
    // dataset without anyone noticing.
    run_python("polymesh_rederive_malformed", R"PY(
answers = {"sigma_box_max": 1.0, "sigma_face_mean": 1.0, "strain_energy": 1.0}
try:
    bad.probe_measured({"kind": "no_such_probe"}, answers)
    raise AssertionError("unknown probe kind must raise")
except ValueError as exc:
    assert "no_such_probe" in str(exc), exc
try:
    bad.probe_measured({"kind": "peak_vm_over_nominal", "nominal": 0.0}, answers)
    raise AssertionError("zero nominal must raise")
except ValueError as exc:
    assert "nominal" in str(exc), exc
print("ok")
)PY");
}
