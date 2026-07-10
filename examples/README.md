# PolyMesh examples

Runnable smoke scripts for the product CLI on the **public geometry fixtures**.
Geometries live under `bench/geometries/public/` (not copied here — keep the
tree small). A symlink `examples/geometries` points at that directory.

| Fixture | Shape | Typical use |
|---------|-------|-------------|
| `unit_box.stl` | 1 m cube | fastest mesh/solve smoke |
| `l_domain.stl` | L-prism | re-entrant corner / graded |
| `plate.stl` | thin plate | skin / feature options |
| `cylinder_prism.stl` | octagonal prism | multi-face solid |

Details: [`bench/geometries/public/README.md`](../bench/geometries/public/README.md).

## Prerequisites

Build the CLI first (from the repo root):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPOLYMESH_WITH_GUI=OFF
cmake --build build -j
```

Override binary or build tree if needed:

```sh
export POLYMESH_BUILD_DIR=/path/to/build   # default: <repo>/build
export POLYMESH_BIN=/path/to/polymesh      # default: $POLYMESH_BUILD_DIR/apps/cli/polymesh
```

## Scripts

All scripts write under `examples/out/` (gitignored) unless you set
`POLYMESH_EXAMPLES_OUT`.

### Mesh only (auto h0)

```sh
./examples/run_mesh_public.sh
# optional: single fixture basename without .stl
./examples/run_mesh_public.sh unit_box
# optional mesher: tet (default) | hex | graded | hexpyr | hexvem
./examples/run_mesh_public.sh l_domain graded
```

Omit `-h` so the CLI uses `resolve_mesh_size` (bbox + sharp-edge density).

### Solve (cantilever-style BCs, auto h0)

```sh
./examples/run_solve_public.sh
# single fixture + optional mesher
./examples/run_solve_public.sh plate tet
```

Default material: \(E = 200\,\mathrm{GPa}\), \(\nu = 0.3\). VTU includes
displacement and von Mises. Open in ParaView:

```sh
paraview examples/out/*.vtu
```

### Manual one-liners

```sh
POLYMESH=./build/apps/cli/polymesh
GEOM=bench/geometries/public

$POLYMESH check $GEOM/unit_box.stl
$POLYMESH mesh  $GEOM/unit_box.stl -o /tmp/box_mesh.vtu
$POLYMESH mesh  $GEOM/l_domain.stl --mesher graded --feature -o /tmp/l.vtu
$POLYMESH solve $GEOM/cylinder_prism.stl -o /tmp/cyl.vtu --mesher tet
$POLYMESH solve $GEOM/unit_box.stl -h 0.1 -o /tmp/box.vtu -E 210e9 -nu 0.29
```

## Notes

- Coordinates and mesh size are **metres**; stresses in VTU are **Pa**.
- Product fills are Cartesian grid (ADR-0015), not true Delaunay — good for
  pipeline smoke, not Tier-1 analytical tolerances.
- GUI path: `./build/apps/gui/polymesh-gui examples/geometries/unit_box.stl`
