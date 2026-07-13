# Geogram subset — what PolyMesh takes

See also [ADR-0025](../../docs/decisions/0025-geogram-cvt-vendor.md) and
[docs/research/geogram-cvt-vendoring.md](../../docs/research/geogram-cvt-vendoring.md).

## Included

| Path | Role |
|------|------|
| `delaunay/Delaunay_psm.{h,cpp}` | Official Geogram PSM: **ConvexCell** (halfspace clip / Voronoi cell), exact **predicates**, Delaunay 2D/3D. Single amalgamation, no Geogram tree deps. |
| `predicates/Predicates_psm.{h,cpp}` | Standalone predicates PSM (lighter if only orient/insphere needed). |
| `LICENSE`, `NOTICE`, per-module `LICENSE` | BSD-3 pin + attribution. |

## Stripped / not vendored

- Full `BrunoLevy/geogram` tree (mesh I/O zoo, image, lua, NL/OpenNL full, parameterization, …)
- GUI / Graphite / vorpalite / vorpaview
- TetGen / Triangle optional backends inside full Geogram
- System package dependency on Geogram (reproducible builds use this tree only)

## How PolyMesh consumes it

- CMake target: `polymesh_geogram` (`third_party/geogram/CMakeLists.txt`)
- Option: `POLYMESH_WITH_GEOGRAM` (default **ON** — PSM is self-contained)
- Product code should prefer thin facades under `src/mesh/` (e.g. `geogram_clip.hpp`) rather than including `Delaunay_psm.h` everywhere.
- Compile defines: `GEO_STATIC_LIBS`, `POLYMESH_WITH_GEOGRAM=1`
- Host flags for numerical robustness (upstream recommendation):
  `-frounding-math -ffp-contract=off` on GCC/Clang for the geogram target only
- Third-party sources compile **without** PolyMesh `-Werror` / pedantic flags

## Upgrade path

1. Re-clone or pull
   `https://github.com/BrunoLevy/geogram.psm.Delaunay` and
   `https://github.com/BrunoLevy/geogram.psm.Predicates`
2. Copy the `*_psm.{h,cpp}` + `LICENSE` files over the existing ones
3. Update `NOTICE` with new commit SHAs and dates
4. Rebuild; run Catch2 `geogram_clip` / unit-cube smoke
5. If API drifts, adjust `src/mesh/geogram_clip.*` only

## Dual hard-block

Median dual-of-tet remains **hard-blocked** until program node **G4** exports
clipped restricted-Voronoi cells as product polyhedra (ADR-0024 Q8 / ADR-0025).
