# Changelog

## 0.1.0 — 2026-08-26

First public Linux release.

### Product

- STEP/BRep import with retained OpenCASCADE topology.
- Geometry-aware tet, hex, transition, prism, hybrid, VEM, Varyhedron, and restricted-CVT mesh paths, with graded tet as the standard preset.
- Linear-elastic solve, adaptive ZZ loop, curved quadratic geometry, stress/displacement/error display, and VTU export.
- Learned mesh advisor with accuracy and calibrated-efficiency objectives, hard feasibility/resource vetoes, and OOD refusal.
- CLI workflows for geometry checks, meshing, solving, diagnostics, headless rendering, calibration, and backend reporting.

### Studio

- Study-first workspace with a dominant 3D viewport, compact setup controls, dedicated FEA results inspector, and Developer/Test Lab workspace on demand.
- Fast, Standard, and Fine mesh presets with research and resource controls behind Advanced setup.
- DPI-aware fonts, controls, panel metrics, and physical-resolution viewport rendering.
- Consolidated display/deformation controls and responsive narrow-window sizing.

### Distribution

- Relocatable CMake install tree and TGZ/CPack packaging.
- `polymesh --version`, consistent binary version identity, relative ONNX Runtime RPATH, and SHA-256-pinned ONNX Runtime download.
- Linux desktop entry, AppStream metadata, scalable icon, bundled runtime notices, and tagged GitHub release workflow.

### Known limits

- Product CAD commands intentionally do not accept STL; Gmsh `.msh` is accepted only by `solve` as an existing volume mesh.
- Product fills are grid based, not constrained Delaunay, and Tier-1 analytical accuracy is not claimed for every product mesh.
- The extreme graded-cylinder h=0.005 sliver chain remains unsolved by the current CG policy; `ellipsoid_boss` retains a small positive-quality boundary-fidelity tail.
- The advisor selects or refuses among measured actions; it does not guarantee a requested error tolerance.
- Linux is the signed-off release target. Windows builds exist but are not part of this release gate.
