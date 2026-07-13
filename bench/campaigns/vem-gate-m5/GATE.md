# M5 VEM gate — campaign results (2026-07-13)

**Verdict: FAIL headline promotion** — `cvt_poly` does **not** yet clear the
gate (health + beat hybrid_zoo on both parts). Product claim stays **tet FE**;
poly VEM **also ships** via G4 clipped cells + `VolumeMesher::kCvtPoly`.

## Campaign

- Path: `bench/campaigns/vem-gate-m5/`
- Parts: plate_hole, cylinder (case JSON + frozen refs)
- Meshers: `cvt_poly` vs `hybrid_zoo`
- `h_scale=5.0`, feature_refine=true, order=1

## Results (tier 0, interior-seeded cvt_poly)

| part       | mesher     | status         | n_elem | n_dof | primary rel_err | health / load_area |
|------------|------------|----------------|--------|-------|-----------------|--------------------|
| plate_hole | hybrid_zoo | ok             | 4608   | 6192  | SCF 0.51        | ok / ok |
| cylinder   | hybrid_zoo | ok             | 17616  | 19635 | SE 0.13         | ok / ok |
| plate_hole | cvt_poly   | solve_suspect  | 190    | 2346  | SCF 0.56        | fail / fail |
| cylinder   | cvt_poly   | solve_suspect  | 402    | 6309  | **SE 0.087**    | fail / fail |

Note: cylinder **strain energy** already beats hybrid_zoo (0.087 < 0.13) at far
lower DOF — but `load_area_ok=false` and plate SCF does not win, so gate fails.

## Substrate landed

- `VolumeMesher::kCvtPoly` — sharp fixed sites + **surface-interior** free seeds
  (`classify_cells_inside`) + Lloyd ρ=1/h³ + clipped Voronoi → `kPolyVem`
- `fea::poly_mesh_to_vem` + VEM constant-strain LSQ centroid stress
- CLI / testlab: `cvt_poly` | `cvt` | `restricted_cvt`

## Next to flip M5 → done

1. Clip Voronoi cells to BRep interior (not only AABB) so load faces sit on the
   solid tip/cap with correct area
2. Better wall-site density on load faces
3. Re-run; require `health_ok` + lower primary rel_err than hybrid_zoo on **both**
   plate_hole (SCF) and cylinder (SE)
