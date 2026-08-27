# Public geometry fixtures

Small closed solids for CAD product smoke and lower-level mesher regression.
The STEP parts carry BRep topology and are accepted by the CLI/GUI product
loader. Legacy STLs remain for focused internal mesh tests; product commands
intentionally reject them. Coordinates are authored in **metres**.

| File | Description | Size scale |
|------|-------------|------------|
| `unit_box.step` | Unit cube, BRep topology (6 faces) | 1 m box |
| `plate+hole.step` | Plate with a through-hole, BRep topology (7 faces) | 68.7 × 27.5 × 30.1 mm |
| `unit_box.stl` | Legacy internal tessellation fixture | 1 m box |
| `l_domain.stl` | Legacy internal tessellation fixture | extent 2 m |
| `plate.stl` | Legacy internal tessellation fixture | plate |
| `cylinder_prism.stl` | Legacy internal tessellation fixture | R=0.5, H=1 |

## Usage

```bash
./build/apps/cli/polymesh check bench/geometries/public/unit_box.step
./build/apps/cli/polymesh mesh  "bench/geometries/public/plate+hole.step" -h 0.008 -o /tmp/plate-hole.vtu
```
