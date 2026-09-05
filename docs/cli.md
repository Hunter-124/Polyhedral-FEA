# `polymesh` command-line reference

The `polymesh` binary (`apps/cli/main.cpp`) is the headless face of the library: it
imports a CAD part, meshes it, solves it, and writes VTU / PNG / JSON artifacts. It is
also the lane external tools use as an independent cross-check of an in-process solve
(Chudware's `polymesh_cli` MCP tool shells out to exactly this binary).

```
polymesh <command> [args]
```

Exit codes: `0` success, `1` a thrown error (message on stderr as `error: …`), `2` bad
usage or a rejected flag value.

## Units

**The solver treats every coordinate as metres**, every modulus and stress as pascals,
and every force as newtons. Nothing in the pipeline inspects a STEP file's declared
units, so a part authored in millimetres must be converted on import:

```
polymesh solve bracket.step --scale 0.001 -o bracket.vtu -h 0.002
```

`--scale` multiplies the exact geometry immediately after the STEP/BREP read, inside
`pipeline::Model::load`, before the tessellation, bounding box, face regions, mirror
frame or any mesh exist (`geom::CadModel::scaled`, an OCCT `gp_Trsf::SetScale` about the
origin with a deep shape copy). Consequently **every other length in the invocation is
in scaled units**: `-h`, `--fix-box` / `--load-box`, the reported bbox and h values, and
the coordinates written into a VTU. With `--scale 0.001` a 2 mm target element size is
`-h 0.002`, not `-h 2`.

A factor of `1.0` (the default) imports the file as authored. A non-positive or
non-finite factor is rejected with exit code 2 rather than silently treated as 1.0.
`--scale` is not accepted for a Gmsh `.msh` input to `solve`: that input is already
discretised, and rescaling its nodes would divorce them from the element sizes the mesh
was built with — convert units in the mesher that wrote the file.

When a factor other than 1.0 is used, the run prints one line before its other output:

```
scale: 0.001 (model units x factor)
```

## Inputs

CAD: `.step`, `.stp`, `.brep`, `.brp` (a retained OCCT BRep — the geometry the mesher,
curved promotion and fidelity checks all query). `solve` additionally accepts a Gmsh 2.x
ASCII `.msh` volume mesh, which it solves directly without meshing. STL input is not
supported.

## Commands

### `check <part> [--scale f]`

Import and validate the geometry, then report vertex/triangle counts of the derived
tessellation and whether a CAD BRep was retained. The cheapest way to prove a file is
readable and, with `--scale`, that its converted size is what you expect.

| Flag | Meaning |
|---|---|
| `--scale f` | uniform import scale (see [Units](#units)) |

### `mesh <part> [flags]`

Geometry- and BC-aware volume mesh, optionally written as a VTU with a per-cell
`quality` array. Prints node/element counts, the resolved `h`, the refinement-plan
summary (geometry seeds, BC seeds, band, `h_fine`, whether curvature came from the BRep
or the tessellation), the mesh-size note, the mesher note, and — when spectral sizing
fired — the retained mode count and energy fraction.

| Flag | Meaning |
|---|---|
| `-h m` | target element size in (scaled) metres; omit or `0` for auto h0 from bbox + feature density |
| `-o out.vtu` | write the mesh (with cell quality) to this VTU |
| `--mesher name` | mesher selection, see [Mesher names](#mesher-names); default `graded` |
| `--skin n` | graded fine skin layers, clamped to ≥ 1 (default 2) |
| `--no-feature` | disable geometry (curvature / thin-wall) grading, which is on by default |
| `--feature` | accepted for back-compat; already the default |
| `--no-spectral` | disable FFT sizing-field trimming and CAD-edge curvature denoise (ADR-0034 baseline behaviour) |
| `--spectral` | accepted for symmetry; already the default |
| `--no-curved` | ship the straight-edged linear mesh instead of the exact curved CAD geometry |
| `--element-tendency t` | element-shape dial in [-1, +1] (hex ↔ fan hybrid ↔ poly VEM ↔ tet) |
| `--max-elems N` | pre-flight element ceiling; `0` selects the 589824 default |
| `--max-dof N` | pre-flight DOF ceiling; `0` selects the 1769472 default |
| `--fix-box x0 y0 z0 x1 y1 z1` | fixture selection AABB; also grades the mesh finer toward it |
| `--load-box x0 y0 z0 x1 y1 z1` | load selection AABB; graded finest |
| `--scale f` | uniform import scale (see [Units](#units)) |

### `solve <part> -o out.vtu [flags]`

CAD input: mesh, select boundary conditions, solve linear elastostatics, write the
displacement/stress VTU. Gmsh input: solve the imported volume mesh directly. `-o` is
required.

Default boundary conditions clamp a slab at min-x and load a slab at max-x (see
[Default BC selection](#default-bc-selection)); `--fix-box` / `--load-box` override that
selection.

| Flag | Meaning |
|---|---|
| `-o out.vtu` | **required** result path |
| `-h m` | target element size in (scaled) metres; omit or `0` for auto |
| `-E Pa` | Young's modulus (default 200e9) |
| `-nu r` | Poisson's ratio (default 0.3) |
| `--mesher name` | see [Mesher names](#mesher-names); default `graded` |
| `--skin n` | graded fine skin layers (default 2) |
| `--no-feature` / `--feature` | disable / (default) geometry grading |
| `--no-spectral` / `--spectral` | disable / (default) spectral sizing |
| `--no-curved` | solve and export the straight-edged linear mesh; the CAD default is curved tet10/hex20 with boundary mids projected onto the BRep (ADR-0035), so this is the opt-out |
| `--p-elevate` | promote to quadratic elements (already implied by the CAD default and by `--adapt`) |
| `--p-elevate-uniform` | promote every tet4/hex8 on tessellated (non-CAD) input too, for order-2 parity with Gmsh peers; implies `--p-elevate` |
| `--element-tendency t` | element-shape dial in [-1, +1] |
| `--adapt n` | ZZ → Dörfler remesh passes (local seeds on the graded path); negative is clamped to 0 |
| `--eta-target η` | stop adapting when the global ZZ indicator η is at or below this value; `0` = off, needs `--adapt` |
| `--bc-grade` | force a-priori BC grading from the default cantilever faces |
| `--fix-box ...6` / `--load-box ...6` | BC / load selection AABBs |
| `--load-dir x y z` | load direction, normalized (default `0 1 0`) |
| `--force N` | total resultant force over the loaded faces (default 1000), applied as a consistent traction ∫Nᵗt dS |
| `--traction Pa` | pressure magnitude instead of a total force; load faces are filtered by normal alignment with `--load-dir` and the resultant is Pa × their area. The last of `--force` / `--traction` wins |
| `--max-elems N` / `--max-dof N` | pre-flight ceilings; `0` selects the defaults |
| `--max-mem GB` | enforced pre-flight solve cap; `0` = auto (70% of currently available memory) |
| `--advisor DIR` | pick mesher / h / adapt / p-order with the learned mesh advisor (`DIR` holds `model.onnx`, `normalization.json`, `clamps.json`); every value is clamped and the decision is logged as JSON. CAD input only |
| `--advisor-objective accuracy\|efficiency` | `accuracy` (default), or calibrated `efficiency`, which minimises predicted mesh+solve time inside a 5% accuracy envelope |
| `--advisor-max-dof N` | with `--advisor`, drop candidate actions whose predicted DOF exceeds N; falls back to the defaults if none fit |
| `--scale f` | uniform import scale, CAD input only (see [Units](#units)) |

### `diag <part> [flags]`

JSON diagnostics: import fidelity against the exact BRep, per-element-type mesh quality,
phase timings, and (unless `--no-solve`) a default cantilever solve. The JSON is written
to `--json` when given and always echoed on stdout.

| Flag | Meaning |
|---|---|
| `-h m` | element size; auto h is additionally capped at bbox_diag/12 so the battery stays quick (the cap is recorded in `mesh_size_note`) |
| `--mesher name` | see [Mesher names](#mesher-names); default `varyhedron` |
| `--json out.json` | also write the report to this path |
| `--no-solve` | skip the diagnostic solve |
| `--no-curved` | diagnose the straight-edged linear mesh |
| `--no-spectral` / `--spectral` | disable / (default) spectral sizing |
| `--max-elems N` / `--max-dof N` / `--max-mem GB` | ceilings, as for `solve` |
| `--fix-box ...6` / `--load-box ...6` | BC / load selection AABBs (they feed the refinement plan too, so `bc_seeds` is a real measurement) |
| `--load-dir x y z` / `--force N` / `--traction Pa` | load specification, as for `solve` |
| `--scale f` | uniform import scale (see [Units](#units)) |

Top-level JSON keys: `part`, `mesher`, `scale` (the import factor this run used),
`import` (vertices, triangles, bbox_diag, cad_brep), `mesh` (h, nodes, elements,
quality_min, quality_min_type, n_inverted_cells, n_below_shape_floor, quality_mean,
geometry_seeds, bc_seeds, geo_curv), `spectral`, `timing_ms`,
`mesh_throughput_elem_per_s`, `fidelity`, `solve` (ran, dof, max_von_mises, max_disp,
global_eta), `mesh_size_note`, `mesher_note`. Every length in the report is in scaled
units.

### `render <part> -o out.png [flags]`

Headless PNG of the same boundary surface the Studio viewport paints — no GL, no window.
It runs the product mesh path, so the image can only show geometry the shipped mesher
actually produced. `-o` is required.

| Flag | Meaning |
|---|---|
| `-o out.png` | **required** image path |
| `-h m` | element size; omit for auto |
| `--mesher name` | see [Mesher names](#mesher-names); default `graded` |
| `--no-curved` | render the straight-edged linear mesh |
| `--no-feature` | disable geometry grading |
| `--no-spectral` | disable spectral sizing |
| `--subdiv N` | subdivisions per quadratic boundary face, clamped to 1…16 (default 8, the viewport's value); linear faces have no interior to subdivide and ignore it |
| `--size WxH` | pixel size (default 1200x900); rejects anything that is not two positive integers up to 16384 |
| `--azimuth DEG` / `--elevation DEG` | orbit camera angles (defaults 35 / 25); the projection is orthographic, so a view is reproducible from these two numbers |
| `--wireframe` | overlay the tessellation triangle edges |
| `--stats out.json` | numeric render report — node/element counts, element-type census, triangle count, covered/silhouette pixels, and facet-normal deviation against the exact BRep normal (omitted for non-CAD input, which has no exact normal) |
| `--scale f` | uniform import scale (see [Units](#units)) |

### `calibrate --out host.json`

Benchmark portable FLOP and byte rates plus a reference mesh time for this host and
write them as JSON (`host`, `flops_per_s`, `bytes_per_s`, `ref_mesh_ms`,
`generated_utc`). `--out` is required; a non-positive benchmark result is an error.

### `backend`

Print the compute backend plus the OpenMP / optimisation summary and exit. No flags.

## Mesher names

`--mesher` accepts every spelling `pipeline::mesher_from_name` knows, and an unknown
name is an error rather than a silent fallback:

| Name(s) | Mesher |
|---|---|
| `hybrid`, `zoo`, `hybrid_zoo`, `mixed` | hex bulk + pyramid skin (ADR-0012 v3) |
| `hybridvem`, `hybrid_vem`, `hybrid-vem` | hex FE bulk + native poly VEM transitions |
| `varyhedron`, `vary` | variable poly packing from CAD (ADR-0021) |
| `cvt_poly`, `cvt`, `restricted_cvt` | restricted CVT → clipped Voronoi poly VEM (experimental) |
| `tet`, `tet_fill` | tet fill |
| `hex` | hex fill |
| `hexvem`, `vem`, `hex_vem` | hex VEM |
| `graded`, `graded_tet` | graded tet |
| `hexpyr`, `transition` | hex core + pyramid skin (ADR-0013) |
| `prism`, `sweep` | Cartesian prism6 wedges along the dominant axis |
| `octa`, `octahedral` | BCC octahedra → tet4 (experimental) |

## Default BC selection

Nodes in a 0.51·h slab at min-x are fixed and the matching slab at max-x is loaded. Only
boundary nodes and faces are ever selected — a selection names a patch of the boundary,
never a volume of material to freeze. When a slab captures too few nodes to behave like
a face (curved parts: fewer than 12 nodes or under 2% of the boundary nodes), selection
falls back to the boundary faces whose outward normal aligns with ∓x/±x within the outer
10%, 25%, then 50% of the x extent.
