---
description: Adversarial audit — verify improvements are real, general, and not gamed
---

You are now the adversary, not the author. Your job is to break the claim that
this phase's improvements are legitimate. Assume the implementation loop cheated
until the evidence says otherwise. Read docs/benchmarks.md §"Anti-cheat /
adversarial audit design" first (the rules this audit enforces), plus
CONTRIBUTING.md §4 "Anti-cheat (sacred)".

Run every check and record a verdict per item as a numbered decision record,
`docs/decisions/NNNN-<slug>-audit.md` (next ADR number), referenced from the
DAG node / phase it gates. `audits/` itself stays holdout-geometry only per
`audits/README.md` — if the audit ran holdouts, commit metrics-only results
there, never reference answers:

1. **Holdout run**: execute the full relevant benchmark tiers on
   `bench/geometries/holdout/` (ask the human to supply/point to it if absent —
   never generate holdouts yourself from geometries you've already seen).
   FAIL if accuracy or speedup degrades >25% relative to the public suite.
2. **Perturbation invariance**: random rigid transforms + unit-system scaling
   of all public geometries; results must be invariant. FAIL on any
   coordinate-dependent behavior.
3. **Parameter sweep**: perturb E, ν, loads ±30%; convergence behavior and
   estimator effectivity must remain stable.
4. **Hardcode hunt**: grep src/mesh/, src/adapt/, src/fea/ for numeric
   literals within 1% of any value in bench/reference/. Review each hit;
   demand a physics justification comment or FAIL.
5. **Estimator honesty**: recompute ZZ effectivity indices on Tier-1 analytical
   cases. FAIL if outside [0.5, 2] — the loop may have tuned the estimator to
   stop early rather than to be accurate.
6. **Test integrity diff**: `git diff <phase-start>..HEAD -- tests/ bench/`
   — list every deleted test, loosened tolerance, or changed reference. Each
   needs a justification in docs/decisions/ or FAIL.
7. **Convergence re-verification**: re-run MMS with 3 fresh random seeds; every
   element type must still hit its theoretical order ±0.2.
8. **Trade-off matrix**: table of every Tier-1/2/3 metric, phase-start vs now.
   Highlight any metric that got worse; judge whether the trade is acceptable
   per docs/spec.md goals, and say so explicitly.
9. **Code review pass**: read the phase's diff specifically hunting for:
   special-casing keyed to benchmark geometry names/sizes, stopping criteria
   tuned to known answers, and silent accuracy-for-speed trades not documented.

Verdict: PASS only if 1–7 all pass and 8–9 raise no unresolved concerns.
On FAIL: write the minimal reproduction of the problem, propose the fix
direction, and hand control back to `/loop` with the audit findings as new
DAG nodes. Do not fix issues yourself in audit mode — separation of duties.
