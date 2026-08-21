# Figure text: the words a reader actually sees

Every generated figure in this repository is read by people who do not train
models and have not read the training code. The text on a figure is written for
them. This file is the single vocabulary all figure generators share, so six
generators cannot invent six different names for the same quantity.

Scope: axis labels, legend entries, titles, subtitles, panel titles, in-plot
annotations, colourbar labels, and any generated table cell or HTML caption.

## Voice

- Plain American English. Complete sentences in subtitles, with a capital
  letter and a period. No British spellings in reader-facing strings
  (`memorizing`, `color`, `normalize` — internal identifiers are not affected).
- Spell the thing out. If a term genuinely has no short plain equivalent, give
  the plain words first and keep the technical term in parentheses after it:
  `pass through the training data (epoch)`.
- Say what the number means, not just what it is named. A reader who cannot
  turn the number into a judgement has learned nothing from the axis.
- No unexplained abbreviation, ever. `MAE`, `AUC`, `BCE`, `DOF`, `p99`, `RMS`,
  `OOD`, `q0.5` do not appear on their own.
- Do not pad. `typical miss predicting a mesh's error` beats
  `mean absolute error of the predicted relative error metric`.

## The unit trap — this is the important one

Every accuracy and cost head is trained on a **log10** target. A miss on those
axes is therefore a distance in **powers of ten**, not a percentage. `0.30` is
not "30% off", it is "off by a factor of two". A bare `0.30` reads as a
fraction and lies in the flattering direction.

So: any axis, legend or caption carrying a log10 distance (the repo calls these
"decades") MUST pair the number with the plain factor.

```python
import figstyle as fs

fs.DECADES_NOTE           # "powers of 10 — 0.30 means off by about 2x"
fs.times_off(0.287)       # "1.9x"        (display; "n/a" on NaN)
```

Use `fs.DECADES_NOTE` in the axis label and `fs.times_off(...)` wherever a
specific value is quoted in prose. Never hand-roll `10 ** x` for display, and
never hand-type the note.

`scripts/advisor/regret.py:decades_to_factor` stays as it is: it returns a
float for JSON records and console tables. `fs.times_off` is the display half.
Both exist on purpose — do not collapse either into the other.

## Glossary — use the right column

| Instead of | Write |
| --- | --- |
| MAE | average miss / typical miss |
| RMSE | average miss, RMS |
| BCE / cross-entropy loss | cross-entropy |
| AUC / ROC AUC | ranking quality (ROC AUC) |
| epoch | pass through the training data (epoch) |
| train (split) | parts it trained on / training parts |
| val, validation (split) | parts it never trained on / unseen parts |
| overfits | much of what it learned is those specific parts |
| decade(s), log10 decades | powers of 10 (+ `fs.times_off`) |
| rel_err | relative error (how far the answer is from the reference) |
| rel_err_rel | relative error, against the case's own median |
| DOF, n_dof | degrees of freedom (unknowns the solver has to solve for) |
| h_rel | cell size, as a fraction of the part |
| p99 | worst 1% |
| chamfer | mesh-to-CAD distance |
| regret | how much worse than the best choice available |
| oracle | the best choice in hindsight |
| fold | cross-validation group |
| fold_std | spread across cross-validation groups |
| macro-mean | averaged evenly across groups |
| quantile / q0.5 | percentile / median |
| OOD | outside the range it was trained on |
| ablation | variant with one piece removed |
| corpus | the data / the training set |
| refusal | the mesher declining to build that mesh |

### Rulings landed during the sweep

These were requested by agents mid-pass and are now in `figstyle.py`. Where the
glossary above and a `figstyle.py` value could disagree, **`figstyle.py` wins** —
call `fs.quantity_label()` / `fs.series()` rather than retyping either.

- Mesher series carry compact legend labels: `fs.series("hex").label` is
  `"hex bricks"`, `hybrid_zoo` is `"hybrid, hex + pyramid skin"`, `cvt_poly` is
  `"Voronoi polyhedra"`. `QUANTITY_LABELS` keeps the longer wording for axes and
  colourbars. The two lengths differ on purpose; do not "fix" one to match.
- `rel_err_rel` is `"relative error, against the case's own median"`. The old
  `"centred per part"` was both jargon and wrong about the grouping: the target
  is the raw value minus that *case's* median over the actions actually run
  (`advisor/dataset.py:CENTRED_HEADS`).
- `pruned_rows` / `pruned_total` are `"worst-fitting rows dropped this run"` and
  `"worst-fitting rows dropped so far"`. Not "outliers": `prune.py` selects by
  worst residual per head, which is a statement about fit, not about the part.
- Plotly axis titles must be `{"text": ...}`. The bare-string form was dropped
  in plotly.js 3.x, so every string-form axis title on the dashboard currently
  renders as nothing at all.

Prefer the existing central tables over a hand-typed string:

```python
fs.quantity_label("h_rel")     # "cell size, as a fraction of the part"
fs.metric_label("rel_err_mae") # "predicted relative error (average miss)"
```

If a quantity is missing from `QUANTITY_LABELS`, **do not** hand-type a label
at the call site and **do not** edit `figstyle.py` yourself — see Ownership.

## Hard never — reject on sight

1. **Never rename a data key.** CSV column names, model head names, JSON/JSONL
   metric keys, ONNX output names and dict keys are provenance. `rel_err_mae`,
   `accuracy_rel_err`, `geo_fidelity_chamfer_mean`, `floor_quantile`,
   `macro_mean_regret` stay spelled exactly as they are in every lookup,
   footer, manifest and record. Only the *displayed prose* changes.
2. **Never change a number, a computation, a threshold or a comparison.** This
   is a copy pass. If a label was wrong about what it plotted, say so in your
   report — do not fix it by changing the maths.
3. **Never soften an honest finding.** `LOST ground`, `REGRESSED`, `excluded`,
   `refused`, `censored` and the failure/exclusion counts stay. Plainer words,
   same verdict, same severity.
4. **Never drop provenance.** Footers, `footer_source`, git revisions, sha256
   digests, record counts and `annotate_n` exclusion counts stay untouched.
5. **Never reformat.** No reflowing, re-quoting, import sorting or style
   churn on lines whose visible text you did not change.
6. **Never widen scope.** Console `print` diagnostics aimed at a developer
   running the script may keep identifiers. Convert the strings that land on a
   figure or in generated HTML.

## Ownership

`scripts/figstyle.py` has one author: the lead. It holds `QUANTITY_LABELS`,
`_METRIC_STATS`, `times_off` and `DECADES_NOTE`.

If you need a new shared label or helper, message the lead over `hub` with the
exact key and the wording you want, and keep working. Do not edit
`figstyle.py`, and do not define a local copy of a shared helper.

## Verification is not optional

Your module writes real files from real artifacts and runs in seconds offline.
Run it, then **look at the output**:

```
python scripts/plot_benchmarks.py
python scripts/plot_truth_independence.py
python scripts/advisor/report.py
python scripts/advisor/plot_evaluation.py
python scripts/advisor/dashboard.py
```

Read the PNG back and confirm the new text actually renders: not clipped at the
panel edge, not overlapping data or a legend, and not silently turned into
boxes. Long replacements are the main risk — a label that reads beautifully in
source and collides with the axis is not done. `fs.assert_glyphs` only checks
strings that are passed to it, so pass new caption text through it.
