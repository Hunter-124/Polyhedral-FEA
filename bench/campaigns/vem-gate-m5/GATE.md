# M5 VEM gate — campaign results (2026-07-13)

**Verdict: FAIL headline promotion** — `cvt_poly` clears **health + load_area
on both parts** but does **not** yet beat `hybrid_zoo` on accuracy. Product
claim stays **tet FE**; poly VEM also ships.

## Campaign

- Path: `bench/campaigns/vem-gate-m5/`
- Parts: plate_hole, cylinder
- Meshers: `cvt_poly` vs `hybrid_zoo`
- `h_scale=5.0`, feature_refine=true, order=1

## Results (latest: VEM τ=0.08 + plate-only wall + cylinder shell sites + OCC snap)

| part       | mesher     | status | n_elem | n_dof | primary rel_err | health / load_area |
|------------|------------|--------|--------|-------|-----------------|--------------------|
| plate_hole | hybrid_zoo | ok     | 4608   | 6192  | SCF **0.512**   | ok / ok |
| cylinder   | hybrid_zoo | ok     | 17616  | 19635 | SE **0.132**    | ok / ok |
| plate_hole | cvt_poly   | ok     | ~5.3k  | ~27k  | SCF **0.545**   | **ok / ok** |
| cylinder   | cvt_poly   | ok     | ~13k+  | ~69k  | SE **0.135**    | **ok / ok** |

Both parts pass health gates. Accuracy still loses:
- plate SCF 0.545 vs hybrid 0.512 (gap ~0.033) — densify / rings / graded size
  often **hurt** SCF; residual headroom is large (~1e-10 vs 1e-6)
- cylinder SE 0.135 vs hybrid 0.132 (gap ~0.0035) — close; τ↓ + shell free
  sites moved SE from ~0.140 → **0.135**

## What landed (code)

1. **`export_rvd_tet_clipped`** — true RVD ∩ Ω (prior)
2. **Load-face expected_area trim** (prior)
3. **Plate-only soft wall inset** — cylinder skips wall pull (stiffened SE)
4. **Cylinder inset shell free sites** — smooth curved wall is not sharp-protected
5. **OCC boundary snap** preferred over STL closest when CAD present
6. **VEM k=1 τ = 0.08** (was 1.0) — recovers compliance; residual still healthy
7. Mild plate in-plane densify near sharp (kept light; aggressive densify regressed SCF)

## Hard-learned (do not re-open without new evidence)

| Attempt | Effect |
|---------|--------|
| Global free sites ≤0.88h | residual risk |
| Graded Lloyd size near hole | residual / SCF worse |
| Polar free-site rings around hole | SCF worse (0.56+) |
| Aggressive half-offset densify | SCF worse |
| Cylinder wall pull / AABB-only | SE or load_area fail |
| Lower VEM τ (0.08) | SE improves; SCF slight help |
| Cylinder shell free sites | SE improves |

## Next to flip M5 → done

1. **Cylinder SE** is almost there (~0.003 gap) — mild further shell densify or
   τ sweep without residual >1e-6
2. **Plate SCF** needs a non-densify lever: hole-wall surface fidelity / face-mean
   dilution (p99 ≈ hybrid while face_mean lags), or VEM recovery — packing
   densify alone is stuck ~0.545
3. Re-run gate; promote only if **both** primary rel_err beat hybrid_zoo with
   health_ok

## History

- AABB-only: cylinder SE 0.087 (won) but load_area fail
- Halfspace domain: cylinder load_area OK, SE 0.41
- RVD tet + load trim: both health OK, accuracy close
- Free 0.9h + plate densify: SCF ~0.546, SE ~0.140
- τ=0.08 + shell + plate-only wall: SCF **0.545**, SE **0.135**
