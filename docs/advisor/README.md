# Learned mesh advisor — document index

The advisor is an action-conditioned outcome model that ranks candidate meshing
actions for a CAD part and load case
([ADR-0027](../decisions/0027-learned-mesh-advisor.md)). Project status and
next work live in [docs/STATUS.md](../STATUS.md); training-host setup in
[docs/training/README.md](../training/README.md).

## Current generation

**Portable-cost cycle, [0012](0012-portable-cost-retrain.md).** Stage-B run 144;
36,010-row dataset over 15 procedural families; 75 inputs; 19,156 parameters;
twelve contract outputs plus three activation taps. Default objective is
accuracy; `--advisor-objective efficiency` uses an optional host calibration.

Shipped artifacts in `bench/advisor/` (loaded by `polymesh solve --advisor
bench/advisor` and `polymesh_testlab --advisor`):

| File | Role |
| --- | --- |
| `model.onnx` | graph: 12 contract outputs + activation taps |
| `normalization.json` | input column order (75), mean/std/impute, vocabularies, output names |
| `clamps.json` | action box, `candidate_grid` (108 measured actions), defaults, `gate_threshold`, `veto_threshold` |
| `ood.json` | Mahalanobis OOD test: columns, center/scale, precision, q0.99 operating point |
| `activation_layout.json` | trunk layout for the activation taps (explain view) |

Evidence for the same cycle:

| File | Content |
| --- | --- |
| `calibration.json` | feasibility calibration, conformal and OOD rates (36,010 rows) |
| `weights.json` | per-head loss weights, Stage A/B schedule |
| `hosts/hunter-pc.json` | committed host calibration (FLOP/s, byte/s, reference mesh) |
| `evidence/campaign_coverage.json` | planned vs recorded campaign rows, every omission |
| `evidence/corpus_evidence.json`, `evidence/geometry_features.csv` | 60-solid descriptor gate |
| `evidence/architecture_benchmark.json`, `evidence/precision_benchmark.json` | rejected four-branch design; training precision throughput |

Other files in `bench/advisor/` (`crossval*.json`, `tolerance_selector*.json`,
`learning_curve.json`, `corpus_evidence.json`, `action_selection.json`,
`baseline_metrics.json`, `throughput.json`, `dashboard.html`) are evidence from
earlier generations; each file's `provenance` block names its dataset.
Earlier-generation datasets and training runs, `bench/advisor/archive-v2/` …
`archive-v7/` and `bench/campaigns/archive-v2/` … `archive-v7/` (removed from
HEAD; `git show ef464dc:<path>`), together with `crossval_v3`–`v6`,
`crossval_nogeo`, `crossval_geofeat` and `tolerance_selector_v4`–`v6` (removed
from HEAD; `git show ef464dc:bench/advisor/<file>.json`), are still cited by the
frozen reports 0006–0011.

## Series

| # | Document | Role |
| --- | --- | --- |
| 0001 | [Architecture](0001-architecture.md) | current input/output contract, inference, deployment |
| 0002 | [Objectives and guardrails](0002-objectives-and-guardrails.md) | losses, curriculum, clamps, gate/veto |
| 0003 | [Training log](0003-training-log.md) | historical milestones M-A1–M-A4 |
| 0004 | [Model card](0004-model-card.md) | current model identity; v2-era decision-quality evaluation |
| 0005 | [Data card](0005-data-card.md) | current corpus and labels; known feature defects |
| 0006 | [Clean-data retrain](0006-clean-data-retrain.md) | superseded cycle report (v3 corpus) |
| 0007 | [Tolerance selector](0007-tolerance-selector.md) | superseded cycle report |
| 0008 | [v4 corpus retrain](0008-v4-corpus-retrain.md) | superseded cycle report |
| 0009 | [v5 mesher-conformity retrain](0009-v5-mesher-conformity-retrain.md) | superseded cycle report |
| 0010 | [v6 exterior-conformity retrain](0010-v6-exterior-conformity-retrain.md) | superseded cycle report |
| 0011 | [v7 curved-geometry retrain](0011-v7-curved-geometry-retrain.md) | superseded cycle report |
| 0012 | [Portable-cost retrain](0012-portable-cost-retrain.md) | **current generation** |
| 0013 | [Solver and meshing survey](0013-solver-and-meshing-survey.md) | methods adopted and deferred for 0012 |

0006–0011 are a frozen historical series: their bodies are not edited, and their
"current"/"shipped" wording refers to their own date.
