# M5 VEM gate — campaign results (2026-07-13)

**Verdict: FAIL headline promotion** — `cvt_poly` clears **health + load_area
on both parts** but does **not** yet beat `hybrid_zoo` on accuracy. Product
claim stays **tet FE**; poly VEM also ships.

## Campaign

- Path: `bench/campaigns/vem-gate-m5/`
- Parts: plate_hole, cylinder
- Meshers: `cvt_poly` vs `hybrid_zoo`
- `h_scale=5.0`, feature_refine=true, order=1

## Results (RVD tet-clip + expected_area load trim)

| part       | mesher     | status | n_elem | n_dof | primary rel_err | health / load_area |
|------------|------------|--------|--------|-------|-----------------|--------------------|
| plate_hole | hybrid_zoo | ok     | 4608   | 6192  | SCF **0.512**   | ok / ok |
| cylinder   | hybrid_zoo | ok     | 17616  | 19635 | SE **0.132**    | ok / ok |
| plate_hole | cvt_poly   | ok     | ~3700  | ~16k  | SCF **0.565**   | **ok / ok** |
| cylinder   | cvt_poly   | ok     | ~13k   | ~46k  | SE **0.138**    | **ok / ok** |

Both parts pass health gates. Accuracy still loses narrowly:
- plate SCF 0.565 vs hybrid 0.512 (gap ~0.05) — feature-local free densify helps
- cylinder SE 0.138 vs hybrid 0.132 (gap ~0.006)

Free-site spacing 0.9×h + plate-like hole-band densify; global denser (≤0.88)
or aggressive feature bands raise residual / DOF budget.

## What landed

1. **`export_rvd_tet_clipped`** — true RVD ∩ Ω: Voronoi cell ∩ nearby domain
   tets (from `tet_fill_surface`). Works for non-convex plate_hole.
2. **Coplanar free-face pairing** post-pass for multi-piece interfaces.
3. **Load-face expected_area trim** in testlab: when free-skin area overshoots
   CAD `expected_area`, keep tip-aligned faces until area matches (honest total
   force).
4. Soft wall inset + light boundary snap.

## Next to flip M5 → done

1. Improve accuracy without blowing residual / DOF budget:
   - plate: denser free sites **only** near hole (feature band that does not
     raise residual above 1e-6)
   - cylinder: recover SE ≤ 0.132 (closer; ~20% relative gap)
2. Re-run gate; promote only if **both** primary rel_err beat hybrid_zoo with
   health_ok.

## History

- AABB-only: cylinder SE 0.087 (won) but load_area fail
- Halfspace domain: cylinder load_area OK, SE 0.41
- RVD tet + load trim: **both health OK**, accuracy close but not yet winning
