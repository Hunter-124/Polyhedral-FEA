# Geogram (vendored subset)

**Status: vendored (G1)** — predicates + ConvexCell via official PSMs.

Not the full Geogram tree. Product poly path still waits on G2–G4 (Lloyd,
constrained sites, clipped export). Dual-of-tet remains hard-blocked until G4.

## Normative docs

- [ADR-0025](../../docs/decisions/0025-geogram-cvt-vendor.md)
- [ADR-0024](../../docs/decisions/0024-advisor-measure-answers.md) Q3 / Q8
- [docs/research/geogram-cvt-vendoring.md](../../docs/research/geogram-cvt-vendoring.md)
- [README.polymesh.md](README.polymesh.md) — included vs stripped, upgrade path
- [NOTICE](NOTICE) — pinned commits / tags

## Layout

```
LICENSE                 # BSD-3 (Inria / Bruno Lévy)
NOTICE                  # pin SHAs
README.polymesh.md
CMakeLists.txt          # target polymesh_geogram
delaunay/               # Delaunay PSM (ConvexCell + predicates + Delaunay)
predicates/             # Predicates-only PSM
```

## CMake

```bash
cmake -S . -B build -DPOLYMESH_WITH_GEOGRAM=ON   # default ON
```

Target `polymesh_geogram` is linked from `mesh` when the option is ON.
