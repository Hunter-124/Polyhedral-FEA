# Public geometry fixtures

Small closed solids for CLI/GUI smoke and mesher development. All are ASCII STL,
under 50 KB, metres units, validated by `geom::load_stl` + `TriSurface::validate`.

| File | Description | Size scale |
|------|-------------|------------|
| `unit_box.stl` | Unit cube `[0,1]³` | 1 m box |
| `l_domain.stl` | L-shaped prism (footprint L, height 1) | extent 2 m |
| `plate.stl` | Thin rectangular plate `2×1×0.2` | plate |
| `cylinder_prism.stl` | Regular octagonal prism (cylinder-ish) | R=0.5, H=1 |

## Usage

```bash
./build/apps/cli/polymesh check bench/geometries/public/unit_box.stl
./build/apps/cli/polymesh mesh  bench/geometries/public/l_domain.stl -h 0.25 -o /tmp/l.vtu
```
