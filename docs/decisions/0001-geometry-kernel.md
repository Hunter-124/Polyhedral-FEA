# ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP, feature-gated; STL always available

- Status: accepted (2026-07-09, GATE 0)
- Decision: D1

## Decision
Link OpenCASCADE directly (native C++, ADR-0007) for B-rep/STEP import and
exact feature queries, behind a default-OFF CMake option `POLYMESH_WITH_OCC`.
The STL path (own loader + discrete curvature/dihedral feature detection) is
always compiled and is the input for the P1–P2 mesher/solver work.

## Alternatives
- STL-only for v1 (SPEC's original recommendation): fastest to the adaptive
  loop, but defers STEP import the owner wants from day one.
- OCC as a hard dependency: exact geometry everywhere, but a heavy C++ build
  taxing every `cargo test` cycle before any physics exists.

## Why
Owner decision at GATE 0: OCC now. Option-gating gives STEP/B-rep without
putting the large OCC build on the critical path of solver development. The
option turns on when P3 feature-analysis work starts consuming exact
geometry. OpenCASCADE is LGPL-2.1, compatible with our AGPLv3 (ADR-0002).
