# ROADMAP — Get PolyMesh off the ground

Master execution plan. **PROGRESS.md** tracks what is done; this file is the
target DAG. Phases map to `docs/phases.md` but GUI is pulled forward (ADR-0006).

## Agent loop protocol (how to finish this)

Every autonomous session follows:

```
1. PLAN   — pick highest-priority unblocked epic from below; write prediction
2. BUILD  — one epic slice (or one vertical user story), tests alongside
3. VERIFY — cmake --build build -j && ctest --test-dir build
            for GUI: also smoke polymesh-gui with a fixture if DISPLAY is set
4. COMMIT — Hunter-124 only, no AI attribution; push master
5. UPDATE — mark todos + PROGRESS.md Done line; if stuck 3× → write blocker
```

Hard rules (CLAUDE.md): patch test sacred, no hardcoded refs in product code,
double only, green suite before push.

**Do not** start GATE-declared phases out of order for frozen baselines.
GUI + mesh/adapt product work may advance in parallel with P2–P5 as long as
GATE 1 solver files stay untouched.

### Parallel tracks

| Track | Goal | Blocks ship? |
|-------|------|--------------|
| **A GUI** | Usable desktop study app | Yes — owner priority |
| **B Mesh** | Credible volume mesh from STL/STEP | Yes |
| **C Hybrid** | Conforming hex/tet/pyramid/VEM | Yes for “hybrid” claim |
| **D Adapt** | Solve → estimate → remesh loop that wins DOFs | Yes for SPEC goals |
| **E Verify** | Competitive benches + e2e on our mesh | Yes for honesty |
| **F Perf** | Threads / AMG / CUDA | No for first usable ship |
| **G Release** | Docs, examples, OSS packaging | End |

---

## Track A — GUI (P6.5 pulled forward)

**Exit:** Owner can open a part, assign fixtures/loads, mesh, solve, see stress
and error, export VTU — without CLI.

| ID | Task | Acceptance |
|----|------|------------|
| A1 | CLI argv: `polymesh-gui [path.stl\|.step]` | Opens model on launch |
| A2 | **Mesh preview** (no solve): Mesh button → boundary quads + element-type colors | Visible before solve |
| A3 | Failed-job UX: clear status, re-enable Solve | Can recover after failure |
| A4 | ZZ error field display mode (η per node or cell) | Third results mode works |
| A5 | Heatmap colorbar + units in viewport | Legend always readable |
| A6 | Wireframe / undeformed overlay toggle | Can see mesh edges |
| A7 | Native file dialog or drag-drop (platform) | Open without typing path |
| A8 | Mesh note + DOF count in UI during/after mesh | Status not empty |
| A9 | Theme polish vs CAD reference (screenshots) | Owner sign-off ⛔ GATE 6.5 |
| A10 | Headless GUI pipeline tests for mesh-only path | ctest covers mesh preview data |

## Track B — Mesh quality (P2 remaining)

| ID | Task | Acceptance |
|----|------|------------|
| B1 | Frontal / constrained Delaunay tet (or documented grid-fill limits) | Tier-1 e2e on *our* mesh for box + L-domain |
| B2 | ~~Quality metrics in mesh note + VTU cell data~~ | Done (mesher note minQ/slivers; VTU `quality` CellData) |
| B3 | Surface snap Jacobian safety (tet + hexpyr) | No inverted elems after snap |
| B4 | STEP path: OCC build option CI job or docker note | Documented how to enable |
| B5 | ~~Fixture geometry suite under `bench/geometries/public/`~~ | Done (≥3 closed STLs + README + load smoke) |

## Track C — Hybrid / features (P3 + P4)

| ID | Task | Acceptance |
|----|------|------------|
| C1 | ~~Conforming hex–pyramid FE (or product path that passes hybrid patch)~~ | Done — all-pyramid expand; patch < 1e-12 |
| C2 | Curvature + thin-wall feature indicators | Sizing reacts on fillet-like meshes |
| C3 | Prism sweep regions (extrusion-detectable solids) | Prism elements in volume_mesh option |
| C4 | VEM k=2 + MMS order check | Order matches theory ±0.2 |
| C5 | Kirsch peak stress @ equal DOF vs uniform tet | Beats baseline (GATE 3 exit) |

## Track D — Adaptive product (P5)

| ID | Task | Acceptance |
|----|------|------------|
| D1 | ~~Dörfler seed remesh~~ | Done (ADR-0014) |
| D2 | ~~Global η stopping criterion (user target)~~ | Done (η target / early-stop) |
| D3 | p-elevation on smooth marked elements (tet10/hex20 promote) | Order increases where smooth |
| D4 | True local h-refine with hanging-node or remesh conformity | Documented + tested |
| D5 | ~~Auto settings: h0 from bbox + feature density~~ | Done (`resolve_mesh_size`, CLI omit -h, GUI note) |
| D6 | Tier-3: ≥5× DOF, ≥3× time vs uniform tet10 baseline | Measured in scoreboard |

## Track E — Verification / competitive

| ID | Task | Acceptance |
|----|------|------------|
| E1 | CalculiX cantilever peer runner green when `ccx` present | JSON under bench/results |
| E2 | PolyMesh labeled scoreboard points for Lamé + Kirsch | docs/bench/scoreboard.md |
| E3 | Holdout geometry protocol (audit) | audits/ stub + instructions |
| E4 | ~~Product mesh → Tier-1 Lamé/Kirsch tests (not only Gmsh import)~~ | Done (box cantilever + cylinder_prism smoke; Lamé tight tol deferred B1) |

## Track F — Performance (P6)

| ID | Task | Acceptance |
|----|------|------------|
| F1 | OpenMP assembly | Speedup on ≥4 cores |
| F2 | CG + AMG (or Eigen iterative) for large N | Solves >50k DOF |
| F3 | CUDA SpMV / batched Ke parity tests | POLYMESH_WITH_CUDA optional |

## Track G — Release (P7)

| ID | Task | Acceptance |
|----|------|------------|
| G1 | ~~README quickstart (CLI + GUI + screenshots)~~ | Done (apt deps, cmake/ctest, CLI unit_box, GUI) |
| G2 | Example models + scripts | `examples/` |
| G3 | API header docs units complete | Spot-check public headers |
| G4 | CI matrix: Linux + format + ctest | Green on PR/master |

---

## Recommended order (critical path to “usable product”)

```
A1 A3 ──> A2 A10 ──> A4 A5 A8 ──> A6 A7
                │
                ├──> B5 B2 ──> B1 E4
                │
                ├──> D2 D5 (adapt UX)
                │
                └──> C1 (hybrid honesty)
                         │
                         v
                   C5 E1 E2 ──> D6 ──> A9 GATE 6.5 ──> G*
```

**First shippable milestone (M1 — “Study app works”):** A1–A5, A8, A10, B2, B5, D2.
**Second (M2 — “Hybrid claim real”):** C1, C5, E4.
**Third (M3 — “SPEC win”):** D3–D6, E1–E2, F1.

---

## Current status snapshot

| Track | Status |
|-------|--------|
| A GUI | M1 core in: argv open, mesh preview, ZZ error, colorbar, failure dismiss, A6 wireframe/undeformed, A7 drag-drop open, A8 mesh note+DOF. Still: A9 theme GATE. |
| B Mesh | Grid tet/hex/graded/hexpyr; B2 quality+VTU cell data; B5 fixtures (≥3). Not true Delaunay (B1 open). |
| C Hybrid | C1 done: hex+pyramid product FE (hex→6 pyramids) patch < 1e-12 (ADR-0013). VEM k=1 only. |
| D Adapt | Seed remesh (ADR-0014) + η target (D2) + auto h0 (D5). Still: p-adapt, local h-refine. |
| E Verify | P1 Tier-0/1/2 + E4 product-mesh box/cylinder smoke. Tight Lamé on product mesh needs B1. |
| F Perf | CPU direct solver only. |
| G Release | README quickstart (G1); examples/ still open (G2). |
