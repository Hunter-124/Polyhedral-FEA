# Public geometry fixtures

Small closed solids for CLI/GUI smoke and mesher development. The STLs are
ASCII, under 50 KB, validated by `geom::load_stl` + `TriSurface::validate`; the
STEP parts carry BRep topology for the CAD path. Every part is authored in
**metres** (raw STEP/STL coordinates are metres, as in `scripts/gen_cad_parts.py`).

| File | Description | Size scale |
|------|-------------|------------|
| `unit_box.stl` | Unit cube `[0,1]³` | 1 m box |
| `l_domain.stl` | L-shaped prism (footprint L, height 1) | extent 2 m |
| `plate.stl` | Thin rectangular plate `2×1×0.2` | plate |
| `cylinder_prism.stl` | Regular octagonal prism (cylinder-ish) | R=0.5, H=1 |
| `unit_box.step` | Unit cube, BRep topology (6 faces) | 1 m box |
| `plate+hole.step` | Plate with a through-hole, BRep topology (7 faces) | 68.7 × 27.5 × 30.1 mm |

## Usage

```bash
./build/apps/cli/polymesh check bench/geometries/public/unit_box.stl
./build/apps/cli/polymesh mesh  bench/geometries/public/l_domain.stl -h 0.25 -o /tmp/l.vtu
```
