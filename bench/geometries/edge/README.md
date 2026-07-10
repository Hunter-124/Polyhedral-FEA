# Edge-case mesh fixtures

Hand-written ASCII STLs used to regression-test grid fill after the
shared-edge ray-parity fix (diagonal voids) and bbox-fitted lattices.

| File | Intent |
|------|--------|
| `unit_box.stl` | Axis-aligned unit cube; volume must be exact |
| `offset_box.stl` | Unit cube at (10,20,30); no origin dependence |
| `thin_plate.stl` | 2×1×0.05 plate; thickness < requested h |
| `slender_beam.stl` | 4×0.2×0.2 beam; high aspect |
| `sphere.stl` | Low-res closed sphere; staircased (volume O(h) error OK) |

Public suite siblings live in `../public/`.
