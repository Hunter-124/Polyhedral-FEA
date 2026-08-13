# ADR-0029: Independent truth, mesh-independent load, and gates that cannot fake success

- Status: accepted (2026-08-13); references replaced and engine fixes shipped, corpus regeneration and retraining in progress
- Decision: D29
- Related: ADR-0023 (measure-first), ADR-0024 (advisor measure answers), ADR-0027 (learned advisor), ADR-0028 (boundary-conformance hardening)
- Evidence: commits `625b26a`, `f238bc3`, `74fbc06`, `8e8806b`, `895f46b`, `eada0a0`, `b37a2df`, `8e8bcb9`, `eb7d8ae`, `76ecdf3`; [`bench/reference/external/`](../../bench/reference/external); [`bench/advisor/corpus_evidence.json`](../../bench/advisor/corpus_evidence.json); [`apps/testlab/load_area.hpp`](../../apps/testlab/load_area.hpp); [`tests/test_load_area_gate.cpp`](../../tests/test_load_area_gate.cpp); [`tests/test_truth_guard.cpp`](../../tests/test_truth_guard.cpp); [`tests/test_run_artifacts.cpp`](../../tests/test_run_artifacts.cpp)

## Context

ADR-0028 established that campaigns are engine-validation gates and that truth
must be rerun before retraining. Running that rerun exposed something larger: the
reference truths themselves were produced by the engine under test, on meshes
ADR-0028 had just proven lose geometry. The audit that followed found four more
defects of one shape — a measurement that reported success without measuring
anything — and one that destroyed the row it was reporting.

Each was found by running a workload and reading what came out, not by reading
code. That ordering is the reason they were found at all.

## Decision

### 1. Truth comes from outside this engine

64 of the 72 corpus references carried `source = "overkill-reference"`: our own
mesher's answer, promoted from `bench/campaigns/advisor-truth-0`. That is
circular, and the meshes involved were the ones ADR-0028 showed lose geometry.

Truth is now produced by a chain that touches neither our mesher nor our solver:
geometry read straight from the case STEP file, meshed by **Gmsh 4.13.1**
(CAD-conforming C3D10, order 2, `HighOrderOptimize=2`), solved by **CalculiX
2.23** (PaStiX direct sparse). `bench/reference/external_truth.py` is the
generator and refuses to write references without `--approved`. The corpus is now
88 `external-gmsh-mesh+calculix-solver` references and 8 closed-form.

The chain was validated against closed form **before** adoption, on the 8 cases
that have one, and the residuals are systematic in sign rather than scattered:

- finite-width SCF: the converged 3D solve sits **+1.78 % to +2.47 %** above the
  2D Howland value, the same sign in all four regimes, which is the
  finite-thickness effect at \(t/a\) of order 1;
- Timoshenko stepped cantilever with Cowper shear correction: deflection
  **+1.11 % to +2.35 %**, energy **+1.03 % to +2.26 %**, self-consistent to
  0.08–0.09 % via \(U = P\delta/2\).

**The corpus's own "analytic" value was wrong.** The four `box_hole_*_c0`
references carried 3.0, the infinite-plate Kirsch idealisation, for parts at
\(d/W = 0.171\text{–}0.196\) where the citable value is Howland (1930) via
Roark ch. 6 / Peterson chart 4.1: \(K_{tg} = 3.091\text{–}3.127\). Against the
handbook the old 3.0 was 2.9–4.1 % low; against the converged 3D solve, 4.2–6.9 %
low. It passed its own \(\pm 10\,\%\) tolerance by luck rather than agreement.

**Our solver was never the suspect.** On one identical Gmsh tet10 mesh
(`channel_s0_c0`, 5,250 nodes, 15,387 DOF) CalculiX and our solver agree to
**3.4e-09** in tip deflection and **6.7e-10** in strain energy, and our CLI
independently reproduces the loaded area to 9 significant digits. The variable was
the mesher.

Evidence: [`external-truth-validate.json`](../../bench/reference/external/external-truth-validate.json)
(closed-form gate and the cross-check),
[`external-truth-findings.json`](../../bench/reference/external/external-truth-findings.json)
(the three headline findings),
[`external-truth-audit.json`](../../bench/reference/external/external-truth-audit.json)
(per-metric before/after for all 96), and
[`results/`](../../bench/reference/external/results) (raw per-rung solver output).

### 2. Applied load may not depend on mesh resolution

A case traction is a pressure, so the assembled resultant was `|traction|` times
the load-face area **the candidate mesh itself selected**. A mesh that failed to
represent part of a loaded CAD face therefore solved a smaller-force problem. The
recorded deficits reach **28.2 %** on ten cases, and because response is linear
while energy scales as \(F^2\), a fractional deficit \(d\) predicts an energy
change of \(1/(1-d)^2 - 1\).

The traction is now rescaled by `cad_rule_area / mesh_selected_area`, where
`cad_rule_area` is the case's own selection rule evaluated on the exact CAD
tessellation rather than on the candidate mesh. The applied resultant is correct
by construction on any mesh. This generalises the pre-existing exact-CAD fallback
from "the selection came back empty" to "the selected area deviates", so it reuses
machinery that was already there. There is one rescale point, which both the
legacy selection and the face-replacement fallback fall through, so double-scaling
is impossible by construction.

**Honest limitation: this corrects the resultant, not the traction distribution.**
A coarse mesh now solves the right problem badly instead of the wrong problem. On
a curved loaded surface the total force is right while its distribution remains
facet-quantised. Fixing the distribution requires more facets on curved loaded
surfaces — a sizing and refinement policy — and is deliberately not folded in
here.

Evidence: `load_deficit_mechanism` in
[`external-truth-findings.json`](../../bench/reference/external/external-truth-findings.json)
carries, per case, the area each side selected, the deficit, both applied
resultants and the \(F^2\)-predicted change beside the observed one, with the
derivation recorded so it can be re-checked rather than trusted.

### 3. One rule has one implementation

The load-selection rule existed twice. For a case specifying
`normal_min_dot = -1` — meaning *load every in-box face* — the CAD side silently
substituted the selector slab's thin axis at `min_dot = 0.7` and dropped the
walls, while the mesh side honoured `-1` literally. `cad_rule_area` was therefore
a 0.7-filtered cap while the mesh loaded the whole in-box set, and since
`cad_rule_area` is the rescale target, the traction was forced onto a region the
case never requested. On `sphere_box_s2_c1` the two sides disagreed by a factor of
**2.3072** and the resultant was scaled down 2.3x: self-consistent, and wrong
relative to intent.

`tlab::load_rule_keeps_normal` is now the only definition of the normal test, with
`tlab::load_rule_filters` for the set-level fallback, and both sides call them.
Divergence is structurally impossible rather than merely repaired.

The 0.7 substitution had a real reason and survives, scoped and named: with the
filter disabled, a transverse end load's traction direction cannot separate an end
cap from the lateral walls sharing the slab, so the slab's thin axis picks *which
CAD faces the face-replacement fallback may substitute*. It is now
`cap_direction`/`cap_min_dot`, documented as cap-identification only, and barred
from `cad_rule_area`.

**A falsifiable prediction validated the diagnosis.** 64 of 96 cases specify
`normal_min_dot = -1`, eight in every family, but a case only diverges if its box
actually contains area the slab filter rejects. `end_face_vs_selected_rel_diff` in
the committed references is exactly `0.0` for every case except the eight
`sphere_box_*_c1`/`_c2`, which reach 0.467 — and those eight are precisely the set
flagged `load_selection_caveat` during the external-truth pass. The eight `tube`
cases were predicted unaffected *before* the fix, because the tube load slab was
sized so the wall ring is empty, and they are bit-identical. A prediction that
could have failed and did not is what makes the diagnosis credible.

Evidence: commit `76ecdf3`;
`supporting_verification.load_selection_caveat_cases` in
[`external-truth-findings.json`](../../bench/reference/external/external-truth-findings.json);
`provenance.load.end_face_vs_selected_rel_diff` in every
[`bench/reference/corpus/*.json`](../../bench/reference/corpus).

### 4. A check that cannot verify must not report success

The load-area health gate compared the selected area against an expected area
**only** when the case supplied `select.expected_area`, and otherwise left
`load_area_rel_err` at its `0.0` default with `load_area_ok` true. A zero relative
error reads as a perfect match, so a mesh missing 28 % of its loaded face recorded
a flawless area check and passed. The gate manufactured confidence in exactly the
runs that were most wrong.

Where it landed is not a coincidence: the only 24 of 96 cases that omit
`select.expected_area` are `sphere_box` and `stepped_shaft`, the only two families
whose loaded surface is curved. The blindness covered the only unguarded cases in
the corpus.

There was a **second, vacuous instance** of the same shape. In the exact-CAD
fallback the reported area was overwritten with the exact CAD area and then
compared against that same value, so \(|A-A|/A = 0\). The genuine measurement was
computed and discarded. The check reported a perfect match while measuring
nothing.

The general rule: **unverifiable is a distinct state and never a pass.**
`LoadAreaStatus` is three-valued — `verified` (an expected area was established
and the tolerance applies), `rescaled_to_exact_cad` (the resultant was corrected,
so the residual is a fidelity measure and is reported without gating), and
`unverified` (nothing establishable, `rel_err` **empty** and serialised as JSON
`null`, never `0.0`). `mesh_selected_area` is always measured and never
substituted, which is what makes the residual non-vacuous. An earlier
`partial_face_selection` state was deleted: it existed only to dodge a comparison
against the wrong quantity, and on `sphere_box` it would have exempted a 66 %
under-load from its own gate — the same bug wearing a different hat.

**The shape recurred twice more in the tooling built to check the work**, which is
why it is recorded as a rule rather than a bug:

- `--stage evidence` overwrote the evidence artifact with an empty one — zero
  cases, zero feature sizes — and only then raised a `TypeError`. It now validates
  inputs before writing anything, cannot emit a zero-case artifact, refuses on
  four named paths with exit 2 and no write, asserts coverage of all 96 cases, and
  stamps `complete=false` with `cases_missing_from_inputs` when `--allow-partial`
  is used (commit `eb7d8ae`).
- `scripts/verify_artifact_guard.py`, written to verify the guard in §5, set
  `skin_layers` to 0, which testlab correctly rejects during grid validation, so
  it aborted before reaching the thing it verified (commit `76ecdf3`).

Evidence: [`apps/testlab/load_area.hpp`](../../apps/testlab/load_area.hpp) states
the policy and its reasoning;
[`tests/test_load_area_gate.cpp`](../../tests/test_load_area_gate.cpp) pins it,
including that an unverifiable area is never reported as `0.0` and never mistaken
for a verified pass. `old_chain_reported_load_area_rel_err` in
[`external-truth-findings.json`](../../bench/reference/external/external-truth-findings.json)
records `0.0` sitting beside a 28.2 % measured deficit — the defect in the data.

### 5. An artifact write may not kill a run

A campaign shard exited 1 and lost the row it had just computed. **The exit code
is what ruled out every crash hypothesis**: 1 is an orderly return from `main`'s
catch-all, where an access violation would be 3221225477, a stack overflow
3221225725, `abort`/`terminate` 3, and an out-of-memory condition would have been
caught and written a `mesh_fail` row. What remained was an unguarded artifact
write inside `run_one`'s own exception handlers — the path that exists to *record*
a failure — turning an ordinary failure row into a whole-process abort that lost
the row it was reporting.

The mechanism is Windows file sharing: `fs::rename` is `MoveFileExW` with
`REPLACE_EXISTING`, which fails with a sharing violation when another process
holds the destination open. The trigger was **our own monitoring** reading
`result.json` while the runner renamed over it. Nothing was wrong with the solve.

It stayed invisible for two sessions because `hub start` captures stdout while the
diagnostic goes to stderr.

`atomic_write` moves to `apps/testlab/run_artifacts.hpp` with a bounded rename
retry; `write_run_json` is `noexcept` and returns success rather than throwing;
`write_warehouse_run` and `write_adapt_trace` are `noexcept` with the
`create_directories` error code checked; the `results.jsonl` write uses the same
check-after-flush idiom. An artifact failure is reported on stderr and the row is
still recorded. `scripts/advisor/rebuild_results.py` can reconstruct
`results.jsonl` from surviving per-run `result.json` files.

Evidence: commit `76ecdf3`;
[`tests/test_run_artifacts.cpp`](../../tests/test_run_artifacts.cpp);
`scripts/verify_artifact_guard.py` obstructs the artifact path and asserts exit 0,
the row still recorded, the failure reported, and no abort.

### 6. Evaluate the rule you ship, on a split that does not leak

The corpus is parametric: `box_hole_s0_c0` and `box_hole_s0_c1` are the *same CAD
solid* under a different load case. Under the historical part-hash split, **672 of
672 validation rows had a training row with an identical (geometry-feature,
action) vector**, differing only in the 7 `case_*` BC columns. Every validation
MAE and regret number published before that was found was measuring interpolation
across three BC variants of a memorised geometry.

Worse, **the chooser being reported was not the chooser being shipped.**
`advisor_policy` (one forward pass at the default action, argmax the policy head)
shipped, while every historical regret number came from `advisor_argmin`
(enumerate the candidate grid, argmin the prediction). They are not equivalent.

Splits are now by group, defaulting to `family` (leave-one-family-out), and
baselines must be *deployable*. Measured leave-one-family-out at a median DOF
budget, the shipped rule changes to feasibility-gated enumeration:
`advisor_gated_0.05` reaches 0.3468 `rel_err` regret at a 10.0 % pick-failure rate
against `advisor_argmin`'s 0.4413 at 22.7 %, and the paired sign test over 6 folds
× 5 seeds is **38W-0L-262T, p = 7.3e-12** — the gate never makes a case worse.
`advisor_argmin` versus the trivial `finest_action` is 117W-136L-47T, p = 0.258:
**ranking alone does not beat the trivial rule; the gate is what earns the win.**
`spend_budget` beats everything and is excluded as not deployable, because it
ranks by *measured* DOF and so can never select an action that failed.

Corpus design is recorded as machine-readable evidence rather than prose:
descriptor distances with each family's `transfer_test_strength` stored adjacent
to its number, the tube load-slab derivation self-checked against the emitted case
JSONs, 1-NN family recovery at 24/24 for the baseline six and 32/32 for all eight
— which is simultaneously why the descriptors cannot transfer under
leave-one-family-out and why they make a strong out-of-distribution signal — and a
power table giving CI half-width 0.0317 at six families against 0.0164 at eight.

Evidence: [`bench/advisor/corpus_evidence.json`](../../bench/advisor/corpus_evidence.json),
regenerated by `scripts/advisor/corpus_evidence.py`;
[model card](../advisor/0004-model-card.md) for the chooser ranking and the paired
tests; [data card](../advisor/0005-data-card.md) §"The split" for the leakage.

### 7. Promotion may only overwrite truth this repo generated

`promote_truth.py` refused to overwrite a metric only when its source was exactly
`"analytic"` — a denylist keyed on the sources that existed when it was written,
which silently stopped protecting anything added later. All 128 externally sourced
metrics were one command away from being replaced by our own overkill values with
their measured tolerances reset.

The rule is now an **allowlist**: promotion may overwrite only `provisional` and
`overkill-reference`, so any external or closed-form source — including one
invented after this commit — is protected the moment it lands. A metric with no
`source` is protected too, because failing closed is the only safe default for
provenance that cannot be established. Refusal is loud and itemised, naming every
protected metric and its source; `--force-overwrite-external` is the single
override and names every reference it would clobber before writing.

`scripts/gen_primitive_corpus.py` was the second, higher-blast-radius writer: it
rewrites all corpus references unconditionally, so re-running it would have
discarded the external answers for first-order surrogates. It enforces the same
allowlist from the same module. `scripts/truth_guard.py` holds that allowlist once
— copying it into each caller would reintroduce the drift it exists to prevent.

Evidence: commit `8e8806b`;
[`scripts/truth_guard.py`](../../scripts/truth_guard.py);
[`tests/test_truth_guard.cpp`](../../tests/test_truth_guard.cpp), whose live-artefact
case asserts that no `analytic` or `external-*` truth in the committed corpus is
promotable.

### 8. Accuracy is re-derived at build time, not frozen into rows

Every campaign row carried a raw `answers` block **and** an `accuracy` block
computed against whatever `bench/reference/` held at solve time. That froze a truth
snapshot into every row, so replacing truth would have meant re-running hours of
campaigns to re-score measurements the rows already contained.
`scripts/build_advisor_dataset.py` now ignores the stored `accuracy` and re-derives
it from `answers` against the current references on every build, mirroring
`evaluate_probe` and the accuracy loop in `apps/testlab/main.cpp`. Changing truth
became a seconds-long dataset rebuild.

Establishing that required fixing the emitter: `sigma_box_max` — the box-windowed
peak von Mises behind the SCF probe — was read by `evaluate_probe` but never
written to `answers`, so those rows were permanently unscoreable. The invariant is
now stated at the write site: `answers` must carry every field `evaluate_probe` can
read.

Evidence: commit `74fbc06`; equivalence was proven over 2,314 archived rows against
the references they were originally scored against before the re-derivation was
relied on.

## Consequences

- Reference values moved a median **3.79 %** and up to **+88.4 %**
  (`stepped_shaft_s1_c2` strain energy). The largest corrections land on the
  families where geometry loss was demonstrated, so the advisor had been scored
  against values wrong by up to a factor of 1.9.
- Tolerances are now derived from measured convergence — reference uncertainty,
  rung-to-rung delta, Richardson error and idealisation bias — mostly 0.02 with 25
  metrics carrying a larger derived band, replacing a hand-picked 0.15.
- A row can now be healthy and still carry a large `load_area_rel_err`: after
  rescaling, that number is a distribution-fidelity measure, not a wrong force.
  Analysis must filter on `load_area_status`, not on `load_area_ok` alone.
- The eight `sphere_box_*_c1`/`_c2` cases must be re-run under §3; the other 88 are
  unaffected.
- Corpus geometry and case JSON remain untracked and regenerated from the
  committed seed table in `gen_primitive_corpus.py`; only the references are
  committed.
- Two new families (`tube`, `perforated_plate`) take the corpus to 96 references.
  This is a power test, not coverage: the measured learning curve over 1–5
  training families is flat, and eight families is where a per-family effect first
  becomes resolvable. `perforated_plate` sits at 1.70 descriptor distance from
  `plate_notch`, below the 2.22 corpus minimum, so it is a weaker transfer test
  than `tube` at 3.18 and a flat slope at eight families must be reported with that
  qualification.
- Evidence artifacts carry a `content_sha256` computed over themselves excluding
  the wall-clock stamp, so reproducibility is checkable rather than asserted.

## Rejected

- **Keeping self-generated truth because it was cheap.** A reference produced by
  the system under test cannot falsify that system.
- **Failing health on a load-area deficit once the resultant is rescaled.** The
  probes are predominantly far-field, where Saint-Venant makes a corrected
  resultant with an imperfect distribution a legitimate measurement. Failing those
  rows would discard usable data for a limitation already recorded.
- **A coverage heuristic as the verification basis.** Comparing the mesh against a
  whole-CAD-face sum conflates a box legitimately clipping a face with a normal
  filter discarding facets of a curved one, and would have exempted a 66 %
  under-load from its own gate. The basis must be rule-consistent.
- **Reporting `0.0` when nothing could be verified.** It is the defect, not a
  convenience.
- **Denylisting known-external truth sources.** A denylist protects only what
  existed when it was written.
- **Quoting any pre-rebuild accuracy magnitude.** The structural findings — the
  leakage, the chooser ranking, the OOD result — do not depend on reference values
  and are expected to survive; the magnitudes do not.
