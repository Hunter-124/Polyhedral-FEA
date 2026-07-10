# ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP, feature-gated; STL always available

- Status: accepted (2026-07-09, GATE 0)
- Decision: D1

## Decision
Bind OpenCASCADE via the `opencascade-rs` bindings for B-rep/STEP import and
exact feature queries, behind a non-default `occ` cargo feature in `geom`.
The STL path (own loader + discrete curvature/dihedral feature detection) is
always compiled and is the input for the P1–P2 mesher/solver work.

## Alternatives
- STL-only for v1 (SPEC's original recommendation): fastest to the adaptive
  loop, but defers STEP import the owner wants from day one.
- OCC as a hard dependency: exact geometry everywhere, but a heavy C++ build
  taxing every `cargo test` cycle before any physics exists.

## Why
Owner decision at GATE 0: OCC now. Feature-gating gives STEP/B-rep without
putting the C++ build on the critical path of solver development. The `occ`
feature turns on when P3 feature-analysis work starts consuming exact
geometry. `opencascade-rs` is LGPL-2.1, compatible with our AGPLv3 (ADR-0002).
