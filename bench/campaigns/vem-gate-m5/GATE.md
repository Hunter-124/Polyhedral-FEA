# M5 VEM gate — campaign results (2026-07-13)

**Verdict: FAIL headline promotion** — `cvt_poly` does **not** yet clear the
gate (health + beat hybrid_zoo on both parts). Product claim stays **tet FE**;
poly VEM **also ships** via G4 clipped cells + `VolumeMesher::kCvtPoly`.

## Campaign

- Path: `bench/campaigns/vem-gate-m5/`
- Parts: plate_hole, cylinder (case JSON + frozen refs)
- Meshers: `cvt_poly` vs `hybrid_zoo`
- `h_scale=5.0`, feature_refine=true, order=1

## Results (latest — surface domain clip + bnd snap)

| part       | mesher     | status         | n_elem | n_dof | primary rel_err | health / load_area |
|------------|------------|----------------|--------|-------|-----------------|--------------------|
| plate_hole | hybrid_zoo | ok             | 4608   | 6192  | SCF 0.51        | ok / ok |
| cylinder   | hybrid_zoo | ok             | 17616  | 19635 | SE 0.13         | ok / ok |
| plate_hole | cvt_poly   | solve_suspect  | 190    | 2304  | SCF 0.55        | fail / fail |
| cylinder   | cvt_poly   | **ok**         | 402    | 8580  | SE 0.41         | **ok / ok** |

### Delta vs first M5 gate (AABB-only)

| metric | first gate | now |
|--------|------------|-----|
| cylinder `load_area_ok` | false (rel ~0.45) | **true** (rel ~0.048) |
| cylinder `health_ok` | false | **true** |
| cylinder SE rel_err | **0.087** (won vs hybrid) | 0.41 (lost — domain clip coarsens boundary) |
| plate `load_area_ok` | false (rel ~1.06) | still false (rel ~1.03) |

Cylinder is the first part to pass the health/load-area half of the gate.
Accuracy vs hybrid_zoo is the remaining SE gap. Plate still fails load-area
because it is **non-convex** (hole): global halfspaces cut opposite material,
so we fall back to AABB + boundary snap (insufficient for load face area).

## Substrate landed (this pass)

- `export_clipped_voronoi(..., DomainClipParams)` — global surface halfspaces
  (majority-vote winding), drop sites outside; domain faces `vglobal=-2`
- Convex-fail fallback: if `n_cells < max(4, n_sites/3)` → AABB export
- Boundary-vertex snap to `TriSurface` (larger budget on AABB fallback)
- Free wall sites soft-inset (never land on surface — zero-thickness cells)
- `poly_mesh_to_vem` drops/repairs non-positive volume debris

## Next to flip M5 → done

1. **Plate (non-convex):** true restricted Voronoi vs volume tet mesh (or
   clip only outer envelope + explicit hole walls as cylinders), not global
   halfspaces of a holed solid
2. **Cylinder SE:** denser free sites / more Lloyd / restore SE ≤ hybrid while
   keeping `load_area_ok` (first gate had SE 0.087 without domain clip)
3. Re-run; require `health_ok` + lower primary rel_err than hybrid_zoo on **both**
   plate_hole (SCF) and cylinder (SE)
