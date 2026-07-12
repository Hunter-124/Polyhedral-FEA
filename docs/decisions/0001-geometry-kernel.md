# ADR-0001: Geometry kernel — OpenCASCADE for B-rep/STEP; STL compare/legacy

- Status: accepted (2026-07-09, GATE 0); **amended 2026-07-12** (ADR-0020)
- Decision: D1

## Decision
Link OpenCASCADE directly (native C++, ADR-0007) for B-rep/STEP import and
exact feature queries, behind CMake option `POLYMESH_WITH_OCC`.

**Amended product policy (2026-07-12):**

- **Product / campaign / GUI study path** requires OCC and a first-class
  `CadModel` BRep (ADR-0020). Build with `-DPOLYMESH_WITH_OCC=ON`.
- **STL** remains compiled for **compare-only** load/export and legacy tests —
  not the primary product geometry source. Generators emit STEP; STL only under
  an explicit compare flag.
- Tessellation from BRep is **derived** (viz, legacy hybrid bridge), not the
  varyhedron primary input.

Option-gating may still keep a light non-OCC unit-test job for pure FEA tests
that never open CAD.

## Alternatives
- STL-only for v1 (SPEC's original recommendation): rejected for product path
  after owner CAD-honesty requirement (ADR-0020).
- OCC as always-on hard dep for every `ctest` binary: still optional for
  non-CAD unit tests; required for product/campaign CI.

## Why
Owner decision at GATE 0 (updated 2026-07-10): OCC may be linked directly when
needed. Owner decision 2026-07-12: product path is BRep-first. OpenCASCADE is
LGPL-2.1, compatible with BSD-3-Clause (ADR-0002).
