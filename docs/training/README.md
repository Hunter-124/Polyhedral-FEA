# Advisor training

How to label a campaign, build the dataset, retrain, validate, export and verify
the learned mesh advisor on any machine. Run every command from the repo root.

| Reference | Link |
| --- | --- |
| Design, feature/action contract | [advisor/0001-architecture.md](../advisor/0001-architecture.md) |
| Label and corpus contract | [advisor/0005-data-card.md](../advisor/0005-data-card.md) |
| Current shipped cycle and its measured results | [advisor/0012-portable-cost-retrain.md](../advisor/0012-portable-cost-retrain.md) |
| Project status | [STATUS.md](../STATUS.md) |

## Prerequisites

| Need | Detail |
| --- | --- |
| C++ targets | `polymesh`, `polymesh_testlab`, `polymesh_tests`, built with `POLYMESH_WITH_ADVISOR=ON` (the default; needs the prebuilt ONNX Runtime) |
| Testlab location | `run_batch.py` launches `build/apps/testlab/polymesh_testlab`, so build into `build/` |
| Python | numpy, torch (CUDA optional; `train.py --device auto` picks it up), onnx, onnxruntime; lightgbm only for `train.py --baseline` |

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target polymesh polymesh_testlab polymesh_tests
```

Windows (MSVC environment wrapper):
`cmd /c "scripts\msvcbuild.bat cmake --build build -j <n> --target polymesh polymesh_testlab polymesh_tests"`.

## Host cost calibration (once per labelling machine)

```sh
build/apps/cli/polymesh calibrate --out bench/advisor/hosts/<host>.json
```

- `<host>` MUST equal the `host` value written inside the file. It is the same
  label testlab stamps on every campaign row (`advisor::local_host_name`).
- Training: the `mesh_work` label is `mesh_ms / ref_mesh_ms` from
  `bench/advisor/hosts/<row host>.json` (`scripts/advisor/cost_labels.py`). Rows
  from a host with no calibration file get no `mesh_work` label.
- Inference: `polymesh solve --advisor <dir> --advisor-objective efficiency`
  reads `<dir>/hosts/<host>.json`; without it the advisor falls back to the
  accuracy objective and says so.
- Commit the calibration with the campaign it labelled. Output fields:
  [cli.md `calibrate`](../cli.md#calibrate---out-hostjson).

## Stages

| # | Stage | Command | Writes (under `bench/`) |
| --- | --- | --- | --- |
| 0 | Corpus (only when the corpus changes) | `python scripts/gen_primitive_corpus.py`, then `--check` | `geometries/corpus/primitives/*.step`, `*.case.json` |
| 1 | Campaign | `python scripts/advisor/run_batch.py --batch <N> --campaign-template <template>` | `campaigns/advisor-batch-<N>-s<K>[-<host-tag>]/results.jsonl`, `advisor/throughput.json`; rebuilds `advisor/dataset.csv` on exit |
| 2 | Dataset | `python scripts/build_advisor_dataset.py` | `advisor/dataset.csv` |
| 3 | Train | `python scripts/advisor/train.py --runs <N>` | `advisor/runs/<NNN>/`, `runs/history.jsonl`, `runs/latest.pt` (resume), `runs/best.pt` (ships); `advisor/normalization.json`, `advisor/clamps.json` |
| 4 | Cross-validation | `python scripts/advisor/crossval.py --epochs 40 --seeds 5` | `advisor/crossval.json` |
| 5 | Calibration / OOD | `python scripts/advisor/calibration.py --seeds 3` | `advisor/calibration.json`, `advisor/ood.json` |
| 6 | Export | `python scripts/advisor/export_onnx.py` | `advisor/model.onnx`, `normalization.json`, `clamps.json`, `activation_layout.json`; refuses on schema drift and checks `ood.json` |
| 7 | Verify | `ctest --test-dir build -R advisor --output-on-failure` | none |

Stage 5 must precede stage 6: export checks the `ood.json` beside the graph.
The shipped model directory is `bench/advisor/` with `model.onnx`,
`normalization.json`, `clamps.json`, `ood.json` and `activation_layout.json`.

### 1. Campaign

| Flag (`run_batch.py`) | Meaning |
| --- | --- |
| `--batch N` | required; names `advisor-batch-<N>-s<K>` |
| `--campaign-template PATH` | grid/tiers/score/resources. Default `bench/campaigns/advisor-batch-template/campaign.json` (batch 1); later batches use `advisor-batch-{2,3,4}-template/` |
| `--parts-glob GLOB` | case JSONs to label; default is the template's parts, else `bench/geometries/corpus/primitives/*.case.json` |
| `--shards K` / `--omp-threads T` | concurrent testlab processes / `OMP_NUM_THREADS` each (defaults 4 / 2); size `K x T` to the machine |
| `--host-tag TAG` | suffix on campaign directories; set it whenever more than one machine labels into the same repo |
| `--skip-truth` | another machine owns the truth campaign |
| `--dry-run` | plan and print only |

- Resume is free: every `(part, cfg_id)` already recorded under any
  `bench/campaigns/advisor-*` directory is skipped, so re-running the same
  command after a crash re-plans against what landed.
- Truth runs first; `run_batch` asks `promote_truth.py --check-promotable` before
  spending order-2 adaptive solves on it.
- Full regeneration after a label-moving mesher change: move the retired
  `advisor-*` directories to `bench/campaigns/archive-<gen>/`, then
  `python scripts/advisor/regenerate_campaign.py --archive <gen> [--from-stage N]`.
  It sequences the committed stage templates through `run_batch` and forwards
  `--parts-glob`, `--host-tag`, `--skip-truth`, `--shards`, `--omp-threads`,
  `--dry-run`.

### Splitting one campaign across machines

1. Give each machine a disjoint `--parts-glob` and its own `--host-tag`;
   exactly one machine runs without `--skip-truth`.
2. Commit each machine's `bench/campaigns/advisor-*-<host-tag>/` results (not
   `runs/`) and its `bench/advisor/hosts/<host>.json`.
3. On one machine, after pulling everything:
   `python scripts/build_advisor_dataset.py` then
   `python scripts/advisor/promote_truth.py --require-all`.

Nothing merges automatically. Training on one machine's half is a
family-truncated corpus.

### 2. Dataset

`build_advisor_dataset.py` takes the union of every `advisor-*` campaign,
excludes `advisor-truth-*` and probe campaigns (`xcheck-*`, `xstl-*`,
`probe-*`), and resolves duplicate pairs by explicit precedence. `--dry-run`
summarizes without writing.

### 3. Train

| Flag (`train.py`) | Default | Note |
| --- | --- | --- |
| `--runs N` | 1 | warm-started runs; stage A -> B switches automatically |
| `--epochs` | 60 | per run |
| `--batch-size` / `--lr` / `--weight-decay` | 256 / 3e-3 / 1e-4 | |
| `--device auto\|cpu\|cuda` | auto | |
| `--precision auto\|fp32\|tf32` | auto | auto selects TF32 on Ampere |
| `--threads` | 4 | torch CPU threads |
| `--split` / `--fold` / `--n-folds` | family / 0 / leave-one-out | family is the only leakage-safe split |
| `--csv`, `--weights` | | dataset / loss-weights overrides |
| `--baseline` | | LightGBM baseline only |
| `--self-test` | | staging rules on synthetic history |

### 4-5. Cross-validation and calibration

| Script | Main flags (defaults) |
| --- | --- |
| `crossval.py` | `--split` (family), `--folds all\|<list>`, `--n-folds`, `--seeds` (5), `--seed0` (1234), `--epochs` (40), `--batch-size` (128), `--learning-rate` (3e-3), `--objective` (rel_err), `--budget-head` (dof), `--include-failures`, `--primary-quantile` (0.5), `--threads` (2), `--out` |
| `calibration.py` | `--split` (family), `--seeds` (3), `--seed0`, `--epochs` (25), `--batch-size`, `--learning-rate`, `--objective`, `--threads`, `--out`, `--ood-out` |

Both train their own per-fold networks; neither reads `runs/best.pt`.

### 6. Export

| Flag (`export_onnx.py`) | Use |
| --- | --- |
| (none) | export `runs/best.pt` (else `runs/latest.pt`) to `bench/advisor/model.onnx` and verify ONNX/torch parity |
| `--checkpoint`, `--output`, `--csv` | overrides |
| `--schema-from-checkpoint` | re-export a shipped checkpoint whose corpus predates the current `dataset.py` |
| `--tiny-fixture` / `--explain-fixture` | rebuild `tests/fixtures/advisor_tiny/` / `advisor_explain/` used by the C++ tests |

### 7. Verify

- `ctest --test-dir build -R advisor --output-on-failure` runs the
  `[advisor]` Catch2 cases (inference, guardrails, parity, explain) against the
  fixtures.
- Live smoke on the exported directory:
  `build/apps/cli/polymesh solve tests/fixtures/parts/plate_hole.step --advisor bench/advisor`
  must exit 0 and log its decision JSON.

## Run discipline

- Record the git SHA of the labelling binary, the campaign snapshot and the seed
  for every run; a model whose provenance cannot be replayed is not shipped.
- Test resume on a short run before starting a multi-day one.
- Report the result the gates measured, including losses; methodology is in
  [ADR-0033](../decisions/0033-a-gate-must-measure-what-ships.md).

The 2026-08 two-machine v4 regeneration plan is archived at
[archive/training/HANDOFF-3080ti.md](../archive/training/HANDOFF-3080ti.md).
