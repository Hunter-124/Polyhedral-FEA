# settings-frontier-1 — campaign-1 close-out

**Finished:** 2026-07-13 · 150 runs · checkpoint `state=finished` tier=2  
**Analysis:** `python3 scripts/analyze_campaign.py settings-frontier-1` →
`PARETO.md` / `PARETO.json` (`ok_rate=100%`, `apply_code_defaults=true` in tooling).

## Survivors (tier-2 keep)

| cfg_id | config (approx) |
|--------|-----------------|
| cfg-e07cd50d | hybrid_vem, tendency=-0.75, feature_refine=true |
| cfg-1b696ce7 | graded_tet, tendency=-0.75, feature_refine=true |
| cfg-dc413db0 | hybrid_vem, tendency=-0.75, feature_refine=false |
| cfg-ff7ccfde | hex, tendency=0, feature_refine=true |
| cfg-02e4c7a5 | hex, tendency=-0.75, feature_refine=true |
| cfg-50b7a344 | hybrid_zoo, tendency=-0.75, feature_refine=false |

## Tooling top-score cfg (global ranking)

`cfg-1ea46b97` — hex, tendency=0, feature_refine=false (fastest composite score
on this part library). **Not applied as product default** — ADR-0023 keeps
**tet FE / hybrid_zoo** as accuracy claim until measure-first campaigns on
frozen STEP refs say otherwise. Hex wins here on speed on smoke_bar/cantilever
skewed grids, not M9 curved STEP accuracy.

## Caveat

Tier-2 rows for survivors show **identical** `rel_err` / DOF across different
meshers on the same part (suspicious collapse). Treat composite ranking as
hygiene for successive-halving machinery, not as a license to switch the
product mesher default. Re-validate any default change on
`varyhedron-baseline-m9` + `vem-gate-m5` first.

## Product default decision (feedback-loop)

| Knob | Tooling suggestion | Applied? |
|------|--------------------|----------|
| mesher | hex | **No** — keep hybrid_zoo / varyhedron path |
| element_tendency | 0.9 | **No** — leave CLI/GUI default |
| feature_refine | false | **No** — feature_refine stays case-driven |
| order | 1 | Already default |

feedback-loop: document-only pass this cycle; node returns to `todo` for the
next campaign.
