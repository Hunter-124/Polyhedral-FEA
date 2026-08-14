# 0007 — "Cheapest mesh within X" is not deliverable yet, and here is the number

Status: measured 2026-08-14 on the 3080 Ti box (`hunter-pc`, gcc Release) at
`d09171d`, dataset `bench/advisor/dataset.csv`, 2,896 rows, sha256
`8a402e910141f5…`. Training track **2b** of
[../training/HANDOFF-3080ti.md](../training/HANDOFF-3080ti.md) §2b. Companion to
[0006-clean-data-retrain.md](0006-clean-data-retrain.md).

## 1. The track asked for a deliverable, not a model

2b names its own acceptance: *"cheapest mesh meeting tolerance X — directly
demoable"*. That needs no new head. The advisor already predicts absolute
`rel_err` and `dof`, so the selector is a filter and an argmin
(`regret.tolerance_chooser`): keep the candidates predicted to meet the
tolerance, take the cheapest. The whole question is whether the error prediction
is good enough **on an unseen family** to keep the promise the button makes.

It is not. Two error models, 12 family-held-out folds, scored by
`regret.cost_at_tolerance`, which reports two numbers because neither is readable
alone: the **violation rate** (the pick's *measured* error exceeded the
tolerance, or it produced no mesh) and the **DOF regret** over the cheapest
measured action that did meet it, averaged over the picks that met it.

The reference is `finest_action` — "ask for the finest mesh offered" — because it
is the deployable trivial rule. A selector that misses the tolerance more often
than that is not shippable however cheap its successes are.

## 2. The measurement

`scripts/advisor/tolerance_selector.py`, output
`bench/advisor/tolerance_selector.json`. `net` is the shipped architecture
trained per fold (40 epochs, seed 1234); `lgbm` is LightGBM on the same masked
rows, included because 0006 §3 measured it as the better cost model by 2.4× on
DOF — so if the selector worked on `lgbm` and failed on `net`, the limit would be
the net.

| tolerance | chooser | violation | DOF regret over cheapest-satisfying |
| --- | --- | --- | --- |
| `rel_err ≤ 0.1` | `finest_action` | **14.8 %** | 1.090 (12.3×) |
| | `net_tol_0.1` | 35.2 % | 0.600 (4.0×) |
| | `net_tol_*_cal` | 27.8–29.6 % | 0.699–0.715 (5.0×) |
| | `lgbm_tol_0.1` | 38.9 % | 0.403 (2.5×) |
| `rel_err ≤ 0.05` | `finest_action` | **31.5 %** | 0.910 (8.1×) |
| | `net_tol_0.05` | 47.0 % | 0.457 (2.9×) |
| | `net_tol_*_cal` | 35.6–37.4 % | 0.498–0.515 (3.2×) |
| | `lgbm_tol_0.1` | 48.5 % | 0.377 (2.4×) |
| `rel_err ≤ 0.01` | `spend_budget` | **76.9 %** | 0.383 (2.4×) |
| | `finest_action` | 81.0 % | 0.237 (1.7×) |
| | every selector | 81.2–90.2 % | 0.000–0.359 |

25–34 cases are reachable per tolerance (a case where no measured action meets
the target cannot test a selector and is excluded).

**The selectors are consistently the cheapest and consistently the least
compliant.** Every one of them beats `finest_action` on DOF by 2–5× among the
picks that satisfy, and every one of them misses the tolerance 1.3–2.6× more
often. That trade is not available to a feature whose entire claim is the
tolerance.

At `rel_err ≤ 0.01` nothing works at all, `finest_action` included: 81 % of
reachable cases are missed by the trivial rule, so 1 % relative error is outside
what this action grid delivers rather than outside what a selector can find.

## 3. A safety margin does not fix it, and the way it fails is the finding

The obvious repair is a conservative margin: require predicted error ≤ tolerance
minus δ decades. `regret.fit_tolerance_margin` sweeps δ over 0.0–2.0 decades and
takes the *smallest* δ whose violation rate is within 10 %.

Fitted leave-one-fold-out — δ for a fold comes from the other folds' held-out
predictions, never from training-set predictions, because LightGBM reproduces its
training rows almost exactly and an in-sample fit returns δ = 0.00 and calls an
uncalibrated selector calibrated:

| model | tolerance | fitted δ | violation on the calibration folds |
| --- | --- | --- | --- |
| net | 0.1 | 2.00 (grid maximum) | 17.8 % |
| net | 0.05 | 2.00 (grid maximum) | 27.7 % |
| net | 0.01 | 2.00 (grid maximum) | 75.5 % |
| lgbm | 0.1 | 2.00 (grid maximum) | 17.8 % |
| lgbm | 0.05 | 2.00 (grid maximum) | 27.3 % |
| lgbm | 0.01 | 2.00 (grid maximum) | 75.5 % |

**The sweep saturates.** No margin on the grid reaches 10 %, for either model, at
any tolerance. At δ = 2.0 decades the admissible set is almost always empty, so
the chooser falls back to "the candidate with the lowest *predicted* error" — and
that fallback still violates 27.8 % at `rel_err ≤ 0.1` where `finest_action`
violates 14.8 %. So the accuracy *ranking* is wrong often enough to lose to a
rule that reads no model at all, which is a stronger statement than "the level is
miscalibrated" and is the reason a margin cannot rescue it.

That both models fail identically also locates the limit: it is not the net.
LightGBM, the better cost model, is no better here (38.9 % vs 35.2 % at 0.1), so
the shortfall is in what `accuracy_rel_err` can be predicted from at all —
consistent with 0003 §"the target, not the estimator, is the limit".

## 4. What ships, and what does not

- **Ships:** the selector, the scorer, and this number.
  `regret.tolerance_chooser` / `fit_tolerance_margin` / `cost_at_tolerance`,
  wired into `crossval.py` and `evaluate.py` so every future advisor run reports
  its violation rate next to its regret. `scripts/advisor/tolerance_selector.py`
  reproduces the table above.
- **Does not ship:** any user-facing "cheapest mesh within X". No CLI flag, no
  C++ query path, no ONNX contract change. Wiring a button whose promise is kept
  63–72 % of the time out of family would be the exact failure 0004 §"not for"
  already forbids — reporting `predicted_*` to a user as an estimate.
- **The next measurement**, before any of this is retried: the tolerance is a
  claim about `accuracy_rel_err`, whose ceiling 0003 already attributes to the
  target rather than the model. A selector cannot outrun that. Either the
  accuracy label gets a trustworthy per-case reference (the truth campaign is
  216/288 pairs, 3 configs incomplete), or the deliverable is restated as an
  *efficiency* claim, where `advisor_efficiency` is already measured and does not
  need to promise a level it cannot certify.

## 5. Provenance

- `bench/advisor/tolerance_selector.json` — 12 folds, seed 1234, 40 epochs,
  `git_revision d09171d`, dataset sha256 `8a402e910141f5…`, 2,896 rows.
- `bench/advisor/crossval_v4_tolerance.json` — the same selectors inside the
  standard 12-fold × 5-seed crossval, where the margin is fitted per fold on
  *training* cases; its calibrated rows are the in-sample variant and are
  superseded by §3 above.
- Labelled corpus is unchanged: this track added no campaign rows, and the rows
  it reads were labelled on the laptop's MSVC build. See
  [../training/ACCESS-hunter-pc.md](../training/ACCESS-hunter-pc.md) §4.1 for why
  that provenance now matters on this box.
