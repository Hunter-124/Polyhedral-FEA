# M5 VEM gate — first campaign result (2026-07-13)

**Verdict: FAIL headline promotion** — `cvt_poly` does **not** beat `hybrid_zoo`
on plate_hole SCF or cylinder strain energy. Product claim stays **tet FE**;
poly VEM **also ships** via G4 clipped cells + `VolumeMesher::kCvtPoly`.

## Campaign

- Path: `bench/campaigns/vem-gate-m5/`
- Parts: plate_hole, cylinder (case JSON + frozen refs)
- Meshers: `cvt_poly` vs `hybrid_zoo`
- `h_scale=5.0`, feature_refine=true, order=1

## Results (tier 0)

| part       | mesher     | status         | n_elem | n_dof | primary rel_err | note |
|------------|------------|----------------|--------|-------|-----------------|------|
| plate_hole | hybrid_zoo | ok             | 4608   | 6192  | SCF 0.51        | health_ok |
| cylinder   | hybrid_zoo | ok             | 17616  | 19635 | SE 0.13         | health_ok |
| plate_hole | cvt_poly   | ok             | 126    | 1350  | SCF 0.81        | coarser; stress via VEM LSQ |
| cylinder   | cvt_poly   | solve_suspect  | 361    | 5778  | SE 0.28         | load_area_ok=false |

## What landed for the gate substrate

- `VolumeMesher::kCvtPoly` — constrained Lloyd + clipped export → `kPolyVem`
- `fea::poly_mesh_to_vem` — PolyMesh → NodalMesh
- `recover_element_centroid_stress` for kPolyVem (constant-strain LSQ) so SCF
  is no longer stuck at 0
- CLI / testlab parse: `cvt_poly` | `cvt` | `restricted_cvt`

## Why not done

ADR-0024 Q10: promote only when **energy/DOF and SCF beat hybrid_zoo** on
**both** parts. Neither metric wins; cylinder fails load-area health on the
bbox-restricted CVT (needs tighter OCC domain clip / more sites — G3/G4 follow-on).

## Next to flip M5 → done

1. Domain = true solid (not bbox): clip sites/cells to BRep interior
2. Site count / h field matching N_pred more tightly on curved walls
3. Load-face filtering for cvt_poly boundary facets
4. Re-run this campaign; require health_ok + lower rel_err than hybrid_zoo on both parts
