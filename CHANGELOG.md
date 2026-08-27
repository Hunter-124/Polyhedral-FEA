# Changelog

## 0.1.1 — 2026-08-27

Multi-platform release. Linux, Windows, and macOS archives are all built and
validated by the tagged release workflow.

### Distribution

- Relocatable release archives for three hosts, published by
  `.github/workflows/release.yml`:
  `polymesh-<version>-linux-x86_64.tar.gz`,
  `polymesh-<version>-windows-x64.zip`, and
  `polymesh-<version>-macos-arm64.tar.gz`.
- Each archive is a self-contained install tree — `polymesh`, `polymesh-gui`,
  `polymesh-webd`, the web frontend and runtime assets under `share/`, and the
  redistributable part of the shared-library closure beside the binaries, reached
  through `$ORIGIN`/`@loader_path` RPATHs. **None of them is a fully static
  build**; each still links the host's system C/C++, OpenGL, and windowing
  libraries.
- `scripts/bundle_macos.sh` and `scripts/bundle_windows.ps1` join the existing
  Linux bundler, and each platform job verifies `--version`, `backend`,
  `polymesh-webd --help`, and the relative-RPATH closure of every shipped binary
  before the archive is cut.
- Per-platform install instructions, checksum commands, and host requirements are
  in [docs/install.md](docs/install.md).

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
- The Windows archive does not bundle the Microsoft C++ runtime. The binaries are
  compiled against the dynamic MSVC runtime, so the Visual C++ 2015–2022
  Redistributable (x64) and an OpenGL 3.3 driver must be present on the host.
- The macOS archive is ad-hoc signed, not notarized. macOS 14 or newer on Apple
  silicon is required, and Gatekeeper quarantine must be cleared
  (`xattr -dr com.apple.quarantine …`) after download.
- There is no Intel-Mac advisor build: ONNX Runtime 1.28.0 publishes an
  `osx-arm64` archive and no `osx-x86_64` one, so arm64 is the only macOS target
  that can carry the learned advisor.
- The 0.1.0 product and accuracy limits below still apply. Its last entry does
  not: Windows and macOS are now part of the release gate, not out-of-band
  builds.

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
