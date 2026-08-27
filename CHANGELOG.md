# Changelog

## 0.1.1 — 2026-08-27

Linux archive. Windows and macOS jobs exist in `.github/workflows/release.yml`
but are skipped (`if: false`): Windows was compiling OpenCASCADE from source
for 70+ minutes, and the macOS runner pool was saturated.

### Distribution

- Relocatable Linux archive published for this tag:
  `polymesh-<version>-linux-x86_64.tar.gz`. Bundler scripts for Windows and
  macOS stay in the tree; those jobs are not part of this tag.
- The archive is a self-contained install tree — `polymesh`, `polymesh-gui`,
  `polymesh-webd`, the web frontend and runtime assets under `share/`, and the
  redistributable part of the shared-library closure beside the binaries, reached
  through `$ORIGIN` RPATH. **It is not a fully static build**; it still links the
  host's glibc, OpenGL, and X11/Wayland.
- The Linux job verifies `--version`, `backend`, `polymesh-webd --help`, and
  the relative-RPATH closure before the archive is cut.
- Install instructions, checksum commands, and host requirements are in
  [docs/install.md](docs/install.md).

### Web

- `polymesh-webd`, a local HTTP server that loads real CAD or STL geometry, runs
  the same import → mesh → solve pipeline as the desktop and CLI applications,
  streams construction and solve telemetry, and serves the static client in
  `web/`. Documented in [docs/webapp.md](docs/webapp.md).
- The browser client reveals the mesh cell by cell in the mesher's own emission
  order (WebGL2 reveal pass in `web/viewport.js`) and plays the advisor's real
  forward-pass activations while the mesher works, rather than animating a
  placeholder.
- Chudware web typography and palette shared with the desktop surface.

### Studio

- Chudware desktop theme: the graphite panel palette, primary/secondary accents,
  Rubik and JetBrains Mono faces, and glassmorphism overlays, with the viewport
  gradient deliberately kept dark for field visualisation.
- Guided study flow through import, material, mesh preset, fixtures and loads, to
  **Mesh preview** / **Solve study**.
- Live rails during a run: the mesh arrives cell by cell through the viewport
  reveal shader as each `MeshStage` is emitted, and the advisor card plays the
  real ONNX activations of every candidate it scored. Missing sources are named
  on screen and skipped; nothing is synthesised.
- Real residual/ZZ convergence plots in a dedicated rail — CG residual history
  and, when the study asked for adaptivity, the per-pass history — drawn from the
  run's own telemetry and still readable after the worker finishes.

### Solver

- Load-conservation checking now uses a magnitude-relative round-off ceiling
  (`fea::load_conservation_tolerance`): `8 · n_terms · eps · |requested|`, with
  1e-9 N kept only as an absolute floor for zero and micro-load cases. Large
  requested resultants are no longer held to a fixed 1e-9 N absolute budget that
  double precision cannot meet.

### Known limits

- The Linux CI build passes `-static-libstdc++ -static-libgcc`
  (`POLYMESH_STATIC_RUNTIME`) only when `libstdc++-static` is available on the
  runner. The result is still a dynamically linked executable that requires a
  host glibc, OpenGL, and X11/Wayland.
- CI and Release jobs have `timeout-minutes` so a hang fails in tens of
  minutes instead of sitting on GitHub's 6 h ceiling. Nothing in CI retrains
  the advisor; inference loads the pinned ONNX artifact.
- Windows and macOS archives are not shipped on this tag. The jobs stay in
  `release.yml` behind `if: false`. Drop those guards when you want those
  archives; Windows first needs a populated vcpkg OpenCASCADE cache or it
  will spend 45–90 minutes compiling OCC from source.

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
