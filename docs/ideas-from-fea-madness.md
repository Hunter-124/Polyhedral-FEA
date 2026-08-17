# Idea harvest: fea-madness (Grok-generated spec, 2026-07-10)

Source: https://github.com/georgedroidnegrotechai-commits/fea-madness
(BSD-3-Clause; contains no implementation — one placeholder cpp and a README
spec. Nothing to port; ideas reviewed and triaged below.)

## Assessment

Most of its scope is already covered here, usually with more rigor:
hybrid variable-order elements (ADR-0003), VEM for polyhedra (ADR-0003),
feature/curvature-driven sizing (P3), ZZ recovery + adaptivity (P5), STEP via
OpenCASCADE (ADR-0001), ImGui desktop app (shipped), GPU acceleration
(ADR-0008), and a verification culture ([docs/benchmarks.md](benchmarks.md) — far stronger than its
"validate accuracy claims" paragraph). Its GUI vision (liquid-glass ribbon)
is superseded by the owner's Interwebz v2 style pivot.

## Worth incorporating

1. **Seed-based Voronoi/Laguerre polyhedral meshing** (P4). The SPEC pins VEM
   elements but not how polyhedral cells get *generated*. Concrete strategy:
   feature-aware seed placement -> Laguerre (power) tessellation -> Lloyd
   relaxation for cell quality. Alternatives to prototype against: dual of a
   tet mesh, octree-based polyhedra. Adopt as the P4 mesher candidates list.
2. **Goal-oriented (adjoint) adaptivity** (P5+). Our plan is energy-norm ZZ +
   Dörfler; adjoint-weighted residuals target a quantity of interest (e.g.
   peak stress at a fillet) instead of global energy error. Add as a P5
   stretch goal / P7+ item — it directly serves "accuracy where it matters".
3. **User-paintable regions** (GUI). Let the user click/paint faces to force
   element type/order/size locally, overriding the automatic classification.
   Our face-picking infra makes this cheap; add when P3 sizing fields exist.
4. **Element type/order coloring + quality heatmap viewport modes** (GUI).
   Visual audit of what the hybrid mesher decided; invaluable for debugging
   adaptivity. Add alongside P3/P5.
5. **Side-by-side comparison mode** (GUI/bench): uniform tet10 baseline vs
   adaptive run on the same part with DOF/time/error readouts — turns the D5
   benchmark claim into a demo feature.
6. **Gmsh as gold-standard comparison mesher** in the audit toolchain
   (import path already planned for P1/P2 testing).

## Rejected / deferred

- CutFEM, peridynamics, smoothed FEM: out of v1 scope (SPEC non-goals stand).
- "Liquid glass" ribbon UI: superseded by owner's Interwebz v2 direction.
- Its proposed directory structure: ours is equivalent and already built.
