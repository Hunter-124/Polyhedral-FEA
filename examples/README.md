# PolyMesh examples

Runnable smoke scripts for the product CLI on the **public CAD fixtures**.
Geometries live under `bench/geometries/public/`; the
`examples/geometries` symlink points at that directory.

| Fixture | Shape | Typical use |
|---------|-------|-------------|
| `unit_box.step` | 1 m cube | fastest BRep mesh/solve smoke |
| `plate+hole.step` | curved plate with through-hole | feature grading and CAD fidelity |

The legacy STLs in the fixture directory remain internal mesher regression
assets. The released product path intentionally accepts STEP/BRep, not STL.
Details: [`bench/geometries/public/README.md`](../bench/geometries/public/README.md).

## Prerequisites

Build the CLI first (from the repo root):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPOLYMESH_WITH_GUI=OFF
cmake --build build -j
```

The scripts use `polymesh` from `PATH`, then fall back to the build tree.
Override either location when needed:

```sh
export POLYMESH_BUILD_DIR=/path/to/build
export POLYMESH_BIN=/path/to/polymesh
```

## Scripts

All scripts write under `examples/out/` (gitignored) unless you set
`POLYMESH_EXAMPLES_OUT`.

### Mesh only (auto h0)

```sh
./examples/run_mesh_public.sh
# optional: single fixture name without extension
./examples/run_mesh_public.sh unit_box
# optional mesher: graded (default) | tet | hex | hexpyr | hexvem
./examples/run_mesh_public.sh plate_hole graded
```

Omit `-h` so the CLI uses `resolve_mesh_size` (bbox + sharp-edge density).

### Solve (cantilever-style BCs, auto h0)

```sh
./examples/run_solve_public.sh
# single fixture + optional mesher
./examples/run_solve_public.sh plate_hole graded
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

$POLYMESH check $GEOM/unit_box.step
$POLYMESH mesh  $GEOM/unit_box.step -o /tmp/box_mesh.vtu
$POLYMESH mesh  "$GEOM/plate+hole.step" --mesher graded -h 0.008 -o /tmp/plate_hole.vtu
$POLYMESH solve $GEOM/unit_box.step -h 0.1 -o /tmp/box.vtu -E 210e9 -nu 0.29
```

## Notes

- Coordinates and mesh size are **metres**; stresses in VTU are **Pa**.
- Product fills are Cartesian grid based (ADR-0015), not constrained Delaunay —
  appropriate for pipeline smoke, not a blanket analytical-accuracy claim.
- GUI path: `polymesh-gui examples/geometries/unit_box.step`
