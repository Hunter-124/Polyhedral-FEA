# 0011 — v7 retrain: authoritative curved CAD geometry

Status: landed 2026-08-18. Supersedes [0010](0010-v6-exterior-conformity-retrain.md).

## Why a whole generation

ADR-0035 made *curved* geometry the shipped default for CAD parts: the solve,
export and render mesh is tet10/hex20 with its boundary mids projected onto the
exact BRep, and a curvature-dominated BRep is filled on a 0.5× lattice. That
changes every mesh-derived quantity the advisor is trained on — element order,
node count, DOF census, `quality_min`, geometry fidelity, mesh and solve time —
so v6 labels describe meshes the product no longer builds. Reusing them would
have trained the chooser on a discretisation that no longer ships.

The harness was also fixed in the same window (`apps/testlab/main.cpp`): it used
to promote and project with a local copy of the logic, so its order-2 rows were
not the product's curved geometry. It now calls
`pipeline::curve_volume_geometry` directly, and every order-2 row records
`curved_volume promoted=… pyramid_split=… projected=… partial=… reverted=…
h_refined=…` in `mesher_note`. Order-1 rows stay linear — that is the sweep
dimension, not an accident.

## What was regenerated

- v6 artifacts archived whole: `bench/campaigns/archive-v7/` and
  `bench/advisor/archive-v7/{dataset-v6.csv,dataset_schema-v6.json,runs-v6/}`.
- Campaign: `scripts/advisor/regenerate_campaign.py --archive v7 --host-tag
  hunter-pc --shards 6 --omp-threads 2` — 4 stages, 3456 new pairs, 68.5 min on
  12 threads. `dataset.csv` rebuilt: 3706 unique rows scanned → **2838 emitted**
  (690 resolution-refusal rows excluded, 178 legacy-schema rows excluded, 235
  retained for the failure head).
- Retrain + export + parity fixture + advisor tests:
  `bash scripts/advisor/land_contract_cutover.sh --build-dir build` — 43-input /
  7-output contract unchanged, 38 candidates per `recommend()`, `model.onnx`
  69.7 KiB, `ctest -R advisor` 11/11 green.
- Decision numbers `bench/advisor/crossval_v7.json`, tolerance
  `bench/advisor/tolerance_selector_v7.json`, calibration/OOD
  `bench/advisor/{calibration,ood}.json`, corpus evidence regenerated.

## Decision quality against v6 (macro-mean regret, family-held-out folds)

| band | objective | policy | v6 | v7 |
|---|---|---|---|---|
| 0.4–0.6 | relative error | advisor_gated_0.5 | 0.4910 | **0.4272** |
| 0.4–0.6 | relative error | advisor_argmin | 0.5374 | **0.4117** |
| 0.4–0.6 | solve time | advisor_gated_0.5 | 0.4518 | **0.4278** |
| 0.4–0.6 | relative error x degrees of freedom | advisor_gated_0.5 | 0.5194 | **0.4189** |
| 0.7–0.9 | mesh-to-CAD worst 1% | advisor_gated_0.5 | 1.2641 | **0.8761** |
| 0.4–0.6 | mesh-to-CAD worst 1% | advisor_gated_0.5 | **0.3495** | 0.9650 |

Accuracy, speed and efficiency regret all improved, and high-band geometry
regret improved by a third. **Mid-band geometry regret got roughly 2.8× worse,
and that is a real finding, not noise.** Regret is measured against the best
action available in each row, and curved geometry makes the order-2 action
dominate on geometry error: `finest_action` reaches 0.0572 mesh-to-CAD worst-1%
regret in that band (v6: 0.0373). The learned policy still under-selects order 2
when the objective is geometry fidelity, so the spread between its choice and
the row optimum widened even though the meshes themselves got much more
accurate. Anyone optimising for geometry fidelity alone should use
`finest_action`; the advisor is the better policy for accuracy-per-cost, not for
geometry alone.

## Calibration, tolerance and OOD

- Pooled failure-head calibration: mean ECE 0.1369, Brier 0.1214, base rate
  0.0874. The shipped 0.5 veto vetoes 4.88% of rows and misses 8.68% of
  failures; `max_missed_0.05` is satisfiable on 24 folds at a 0.95 threshold
  with a 1.87% veto rate.
- Conformal intervals stay wide relative to their nominal level (0.9 → ±0.324
  decades achieving 0.284), so the interval is reported, never trusted as a
  guarantee.
- OOD gate refitted over the 15 offline CAD descriptors: threshold 5.034 at a 1%
  in-sample false-alarm rate, **83.3% held-out-family detection** across 12
  folds (v6: 100%). The wider v7 corpus makes families less separable in that
  space; the gate still abstains rather than imputes.
- Tolerance selector: `max_violation` 0.1 — it still violates the tolerance more
  often than `finest_action`, so the standing verdict is unchanged and **no
  selector ships**.

## Provenance

- Campaign binary: `build/apps/testlab/polymesh_testlab` built 2026-08-18
  02:16:19Z, from the same commit range as the curved-geometry landing
  (`b406e2e…6d36883`). Every row uses an explicit `-h`, so the later auto-sizing
  change (`d2d9a15`) cannot have affected any label.
- 30 cross-directory `CAMPAIGN_PRIORITY` collisions were reported by the
  dataset rebuild and resolved by scan order, as in v6; 6 of them are identical
  results with nothing to break.
- References untouched: every corpus reference carries an `analytic` or
  `external-gmsh-mesh+calculix-solver` source, so the truth gate had nothing to
  promote.
