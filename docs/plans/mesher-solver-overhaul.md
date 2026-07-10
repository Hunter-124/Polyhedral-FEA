# Plan: Mesher / Solver Accuracy + Performance Overhaul

**Status:** Ready for approval · **Mode:** monolithic DAG + parallel subagents  
**Handoff artifact (in-repo, write on execute):** `docs/plans/mesher-solver-overhaul.md`  
**Owner priority:** (1) hybrid zoo + graded tet fixes, (2) shared perf, (3) octahedral experiment, (4) solver polish

This plan is the source of truth for this epic. Any agent/harness that picks it up should:

1. Read this file + `docs/ROADMAP.md` + `docs/progress.md` + ADRs 0012/0013/0015/0016  
2. Run `graphify query "graded hybrid mixed_fill assembly snap"` if `graphify-out/` exists  
3. Execute unblocked DAG nodes in parallel worktrees / subagents  
4. After each landed node: green `ctest`, commit+push `master` (Hunter prefs), update progress  

---

## Context

Screenshots (same part, hole plate / similar):

| Mode | Observation |
|------|-------------|
| **hybrid zoo** | Dense Kuhn-tet skin around hole; jagged black ring of diagonals; slower than hex |
| **graded tet** | ~3.55M elems / ~696k nodes reported; still poor hole silhouette; slower than hex |
| **hex (grid)** | ~213k elems / clean structured quads; best visual + fastest of the three |

Product claim (SPEC / ADR-0012): hybrid zoo and graded tet should beat uniform lattice DOF/time for accuracy-critical geometry. Today they are **slower and less accurate-looking** than plain hex grid fill. ROADMAP tracks B–F are marked “done,” but product mesh quality is still Cartesian-grid limited (ADR-0015) and several paths have structural correctness gaps that unit tests never catch.

### Why hex currently wins (root causes)

1. **Graded 2:1 is topologically nonconforming (CRITICAL accuracy bug)**  
   In `graded_tet_fill_surface` (`src/mesh/src/hybrid_fill.cpp`), coarse cells emit Kuhn tets at fine-index step 2 (8 corners only). Fine neighbors emit 2×2×2 step-1 Kuhn cubes that introduce **edge midpoints and face centers on the shared plane**. Those mid-edge nodes hang: they appear only on the fine side.  
   `check_tet_fill_geometry` / `NodalMesh::check_validity` only check positive volumes and index range — **not face matching**. Catch2 graded tests never assert conformity. FE results on graded meshes are therefore not trustworthy.

2. **Hybrid zoo bulk is not real hex when mixed**  
   `fea/assembly.cpp`: if *any* tet/pyramid exists, every hex8 is assembled as the same Kuhn 6-tet PL split as the skin. Consequences:  
   - Bulk pays ~6× tet assembly cost while losing isoparametric hex locking resistance  
   - Skin Kuhn tets put face diagonals on free surfaces → jagged hole silhouette (Image #1)  
   - Visual/DOF density on boundary is worse than hex quads (Image #3)

3. **Hybrid feature/seed marking is O(cells × features/seeds)**  
   `mixed_fill.cpp` L192–206: per-cell `geom::distance_to_features` + nested seed loop. Graded already fixed this with ball stamping (`mark_seed_cells` / `mark_feature_cells`). Hybrid still has the slow path.

4. **Brute-force surface snap is shared pain**  
   `closest_on_surface` is O(N_tris) per query; multi-pass snap is O(passes × boundary_nodes × tris). Hybrid/graded use aggressive snap (0.85–0.9 h, 4–6 passes) so they pay more than hex.

5. **Graded RAM / node table**  
   Full dense fine-index slot array `(2nx+1)(2ny+1)(2nz+1)` even when only a thin skin is refined. On large grids this is memory- and TLB-hostile vs hex’s sparse `std::map` of used lattice nodes.

6. **Over-aggressive a priori skin/feature bands (product defaults)**  
   Hybrid: `feat_band = 2.5 h`, curvature seed flood, `s_band = 2 h`. Graded was recently capped (≤192 seeds, skin_cap) but still converts large volumes to fine Kuhn when the hole is a high-κ feature. Result: element counts blow past hex for little boundary-fit gain under ADR-0015 staircasing.

7. **Solver is “done” (F1–F3) but weak for large hybrid/graded systems**  
   CG + diagonal preconditioner only; no AMG/ILU. Hybrid Kuhn expansion inflates assembly; graded multi-million tets OOMs/slows before physics quality matters.

### Constraints (do not break)

- GATE-1 pure hex8 / pure tet formulations on *imported* structured meshes stay frozen.  
- ADR-0015 honesty: product fill remains Cartesian until a true Delaunay/CAD mesher ships — do not market “boundary-fitted” after cosmetic snap.  
- Double only; no AI attribution in commits; green full suite before push.  
- Prefer geometric conformity via transition elements (SPEC); hanging nodes only if constrained or VEM-owned.

---

## Goals / success metrics

After this epic, on public fixtures (`bench/geometries/public/*`, hole/cylinder-class parts):

| Metric | Target vs current hex baseline at same user `h` |
|--------|--------------------------------------------------|
| Mesh wall time (hybrid, graded) | ≤ 1.5× hex (goal ≤ 1.0× after BVH+stamp) |
| Free DOF at equal energy / peak stress proxy | Graded/hybrid ≤ hex DOFs for same error band on Kirsch-class (when analytic available) **or** equal-DOF error ≤ hex |
| Mesh validity | Face-conforming tet/hybrid; patch test max ‖u−u_exact‖_∞ < 1e-10 on mixed lattice |
| Graded element count | No multi-million tet floods on moderate plates at default GUI `h`; skin ≤ ~2–3 coarse layers |
| Hybrid free-surface look | Prefer quad/pyramid skin over Kuhn diagonal rings |
| Octa experimental | Meshes + solves smoke; no claim of superiority |

Deliverable artifacts:

- `docs/plans/mesher-solver-overhaul.md` (copy of this plan, tracked)  
- New ADR(s): graded conformity strategy; hybrid FE path revision (amend 0012)  
- `bench/` mesher scoreboard JSON rows: time, elems, nodes, minQ, snap residual, optional solve energy  
- Catch2: face-conformity, hanging-node absence, hybrid stamp parity, octa smoke  

---

## Recommended approach (not all alternatives)

### Graded tet (highest priority accuracy)

**Ship conforming 2:1 red-green style templates on the Cartesian lattice**, not hanging nodes and not “refine everything to fine.”

Algorithm sketch (in `hybrid_fill.cpp` / possibly new `graded_templates.hpp`):

1. Coarse-primary lattice at target `h` (keep current classify + skin/feature/seed marking).  
2. Mark `is_fine` as today.  
3. **Transition closure:** for every coarse cell that shares a face/edge with a fine cell, mark it as `is_transition` (not full 2×2×2 unless already fine).  
4. Emit:
   - fine cells → 8 Kuhn cubes (unchanged)  
   - deep bulk (no fine/transition neighbor) → 1 Kuhn cube step 2 (unchanged)  
   - transition cells → **fixed tet templates** that include the mid-edge nodes required by each fine-adjacent face so every triangular face matches exactly once  

Start with the common case (1–3 faces adjacent to fine; “planar” 2:1). If a cell has a pathological adjacency pattern, fall back to full fine 2×2×2 (safe, local over-refine).

**Secondary path (fallback if templates slip schedule):** generate uniform coarse Kuhn mesh then `local_refine_tets` (LEB/LEPP, ADR-0016) on marked cells — correct but slower; keep as reference oracle in tests.

**Must-add test:** build face map (sorted 3-node keys); every interior face appears exactly twice; no unpaired faces. Fail graded if unpaired > 0.

### Hybrid zoo (highest priority product quality)

Redesign product hybrid toward SPEC intent (true multi-type + geometric conformity):

| Zone | Element | FE formulation |
|------|---------|----------------|
| Deep bulk | hex8 | **Isoparametric trilinear** (not Kuhn PL) |
| One-cell transition ring | pyramid5 | Existing tet-split pyramid stiffness (ADR-0013) |
| Optional curved free-surface skin | tet4 Kuhn **or** keep pyramid bases on boundary | Snap on free nodes |

Implementation options ordered by risk:

1. **Preferred product path (H2):** rebuild `mixed_fill_surface` zones:  
   - `dist >= skin_layers+1` → hex  
   - `dist == skin_layers` (or hex–tet interface cells) → pyramid ring from hex faces toward tet  
   - `dist < skin_layers` OR feature/seed → Kuhn tet  
   Conformity = pyramid bases match hex quads; pyramid triangles match Kuhn diagonals on the tet side (pick diagonal convention carefully — reuse ADR-0013 expand lessons).  

2. **Interim (H1):** keep current hex+tet topology but:
   - stamp features/seeds like graded (perf)  
   - stop Kuhn-assembling bulk hex; instead expand only interface hexes to pyramids (or full expand like `kHexPyramid` for solve mesh while display keeps multi-type)  

3. **Reject for product:** continuing “hybrid = Kuhn-everything in assembly” — it cannot beat hex.

### Shared infrastructure (unlocks both)

- **BVH / grid-hash closest point** for `closest_on_surface` (`surface_project.cpp`)  
- Shared **feature/seed stamp** utility extracted from graded into `mesh/` (used by mixed + graded + sizing)  
- Optional sparse node maps for graded (hash map instead of dense `int32` slot cube when fill fraction low)

### Octahedral experiment (low priority, parallel)

New `VolumeMesher::kOctahedral` + `octa_fill_surface`:

- BCC / body-centered Cartesian: lattice nodes + cell centers  
- Each pair of face-adjacent cell centers + shared face → octahedron (6 verts) **or** regularize as poly-VEM cell  
- Simpler v1: emit octahedra as `kPolyVem` (k=1) with 8 triangular faces; fallback split each octa → 4 tets if VEM path is heavy  
- GUI button + CLI flag; mesher note marks **experimental**  
- No accuracy claims; smoke mesh+solve on unit box only

### Solver (after mesh paths stable)

- Incomplete Cholesky or AMG-lite for CG when free DOF > ~20k  
- Cache hybrid-mesh flag once per assemble (already thread_local; ensure OpenMP-safe)  
- Avoid expanding every hex to 6 tets in product hybrid after H2  

---

## Critical files

| Area | Paths |
|------|--------|
| Graded | `src/mesh/src/hybrid_fill.cpp`, `src/mesh/include/mesh/hybrid_fill.hpp` |
| Hybrid | `src/mesh/src/mixed_fill.cpp`, `src/mesh/include/mesh/mixed_fill.hpp` |
| Snap | `src/mesh/src/surface_project.cpp`, `src/mesh/include/mesh/surface_project.hpp` |
| Classify | `src/mesh/src/grid_classify.cpp`, `src/mesh/include/mesh/grid_classify.hpp` |
| Pipeline / GUI | `src/pipeline/src/scene.cpp`, `src/pipeline/include/pipeline/scene.hpp`, `apps/gui/main.cpp`, `apps/gui/widgets.cpp` |
| Assembly | `src/fea/src/assembly.cpp` |
| LEB oracle | `src/mesh/src/local_refine.cpp` |
| Quality | `src/mesh/src/quality.cpp` |
| Tests | `tests/test_graded_fill.cpp`, `tests/test_mixed_fill.cpp`, `tests/test_quality.cpp`, new `tests/test_mesh_conformity.cpp`, new `tests/test_octa_fill.cpp` |
| ADRs | amend `docs/decisions/0012-hybrid-graded-tet.md`; new `0018-graded-conformity.md`; optional `0019-octahedral-experiment.md` |

### Reuse (do not reinvent)

- `mark_seed_cells` / `mark_feature_cells` patterns from graded → extract shared  
- `local_refine_tets` LEB as conformity oracle / fallback  
- `expand_mixed_hex_to_pyramids` / ADR-0013 face ordering  
- `kCubeTets` Kuhn convention (keep one diagonal policy project-wide)  
- `summarize_tet4_quality`, `snap_boundary_nodes` + Jacobian unsnap  
- `make_bbox_grid` / `classify_cells_inside` (OpenMP uint8 mask)  
- Bench JSON schema under `bench/competitive/`

---

## Monolithic DAG (parallel execution)

Nodes are landable PRs/commits. Edges = must-finish-before. Same rank = **safe to parallelize** in worktrees/subagents if they don’t thrash the same files (or serialize file ownership as noted).

```text
                         ┌──────────────────────┐
                         │  M0  Handoff doc +   │
                         │  mesher scoreboard   │
                         │  harness (metrics)   │
                         └──────────┬───────────┘
                                    │
              ┌─────────────────────┼─────────────────────┐
              │                     │                     │
              v                     v                     v
     ┌────────────────┐   ┌─────────────────┐   ┌──────────────────┐
     │ G0 Face-map    │   │ S0 BVH/grid     │   │ H0 Extract stamp │
     │ conformity     │   │ snap accel      │   │ util (from       │
     │ test harness   │   │ (surface_proj)  │   │ graded) + use in │
     │ (red first)    │   │                 │   │ mixed_fill       │
     └───────┬────────┘   └────────┬────────┘   └────────┬─────────┘
             │                     │                     │
             v                     │                     v
     ┌────────────────┐            │            ┌──────────────────┐
     │ G1 Graded 2:1  │            │            │ H1 Hybrid zone   │
     │ transition     │◄───────────┘            │ retune defaults  │
     │ templates      │   (snap used by both)   │ + stamp + thinner│
     │ (CRITICAL)     │                         │ skin             │
     └───────┬────────┘                         └────────┬─────────┘
             │                                           │
             v                                           v
     ┌────────────────┐                         ┌──────────────────┐
     │ G2 Graded perf │                         │ H2 True hybrid   │
     │ sparse nodes,  │                         │ hex + pyramid    │
     │ quality note,  │                         │ ring + tet skin; │
     │ seed policy    │                         │ isoparam hex FE  │
     └───────┬────────┘                         └────────┬─────────┘
             │                                           │
             └───────────────────┬───────────────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
              v                  v                  v
     ┌────────────────┐ ┌────────────────┐ ┌────────────────┐
     │ O1 Octa fill   │ │ V1 CG precond  │ │ E1 Scoreboard  │
     │ experiment     │ │ (ILU/AMG-lite) │ │ remeasure all  │
     │ (parallel OK)  │ │ after mesh OK  │ │ meshers + docs │
     └────────────────┘ └────────────────┘ └────────────────┘
```

### Node specs

| ID | Title | Owner files | Acceptance | Depends |
|----|-------|-------------|------------|---------|
| **M0** | Scoreboard harness | `bench/mesher/`, thin CLI or extend `polymesh` | JSON: mesher, h, time_ms, elems, nodes, minQ/slivers, snap_max, note; run on unit_box + cylinder_prism + plate | — |
| **G0** | Face conformity tests | `tests/test_mesh_conformity.cpp`, maybe `mesh/quality` helper | Graded on unit box **currently fails** or detects unpaired faces; documents bug; hex/uniform tet pass | M0 optional |
| **S0** | Snap acceleration | `surface_project.*` | ≥5× faster closest queries on large STL in microbench; identical snap residual within 1e-12 on fixtures | — |
| **H0** | Shared stamp + hybrid use | `hybrid_fill` extract, `mixed_fill` | mixed feature/seed marking O(seeds·ball); unit test parity with distance loop on small grid | — |
| **G1** | Graded transition templates | `hybrid_fill.*`, ADR-0018 | Face map closed; positive volumes; patch/MMS smoke not worse; no hanging mid-edge free DOFs | G0 |
| **H1** | Hybrid product defaults + stamp | `scene.cpp`, `mixed_fill`, GUI defaults | Hybrid mesh time on plate ≤ ~2× hex; no full-volume tet skin | H0 |
| **G2** | Graded perf/quality polish | `hybrid_fill`, pipeline seeds | Sparse node table or prove dense OK; quality in mesher note; elem count sane on public STLs | G1, S0 |
| **H2** | Hex + pyramid + tet hybrid FE | `mixed_fill`, `assembly`, ADR-0012 amend | Bulk hex isoparametric when only pyramids/tets at skin; constant-strain patch < 1e-12; hole silhouette competitive with hex | H1, S0 |
| **O1** | Octahedral experiment | new `octa_fill.*`, scene, GUI | Mesh+solve unit box; experimental label; no default | — (parallel anytime after M0) |
| **V1** | Solver preconditioning | `solve.cpp`, tests | CG iterations ↓ on ≥50k free DOF vs diagonal; no GATE-1 drift | H2 or G1 preferred |
| **E1** | Close the loop | docs, progress, ROADMAP track | Scoreboard before/after table; ROADMAP new track H or reopen B/C; progress Done bullets | G2, H2, O1, V1 |

### File ownership (to avoid merge thrash)

| Lane | Exclusive files | Parallel with |
|------|-----------------|---------------|
| Graded lane | `hybrid_fill.*`, graded tests, ADR-0018 | S0, H0, O1 |
| Hybrid lane | `mixed_fill.*`, assembly hybrid branch, mixed tests | S0, G0, O1 |
| Infra lane | `surface_project.*`, stamp util header if new file | after extract settled |
| Octa lane | new files only + thin scene/GUI enum edits | everything until scene enum merge |
| Bench lane | `bench/mesher/**` | all |

When two lanes need `scene.cpp` / `VolumeMesher` enum: land enum additions first in a tiny **M0b** commit (`kOctahedral` reserved, no behavior).

---

## Implementation notes (for coding agents)

### G1 template detail (minimum viable)

For a coarse cell with **exactly one** face adjacent to fine cells:

- Insert 4 edge midpoints + 1 face center on that face (fine lattice nodes already exist if fine neighbor emitted — **must use same node_at indices**).  
- Subdivide the coarse cube into tets that use those nodes so the interface triangulation equals the fine side’s Kuhn face pairs.  
- Literature: red-green refinement / “transition cube” tet tables; implement tables as `constexpr` arrays with unit tests per configuration bit-mask (6 bits face adjacency → 64 cases; many map to rotations of a few base cases).

### H2 conformity diagonal policy

Pyramid tet-split uses base diagonal 0–2. Kuhn skin uses `kCubeTets` diagonals. The tet-side face adjacent to a pyramid **must** use the same diagonal as the pyramid base split. Add a single helper `kuhn_face_diagonal(face_id)` used by both emitters.

### Assembly change for H2

```cpp
// Remove "any tet ⇒ all hex Kuhn" global.
// Instead: pure hex mesh → isoparam hex (GATE-1).
// Hybrid with pyramids: hex stays isoparam; pyramid/tet as today.
// Only if hex shares a face with tet without pyramid buffer → assert/fail mesher.
```

### Profiling commands (agent verify)

```bash
cmake --build build -j$(nproc)
# mesher micro
/usr/bin/time -f '%e s' ./build/apps/cli/polymesh mesh bench/geometries/public/plate.stl -h 0.05 --mesher hybrid
/usr/bin/time -f '%e s' ./build/apps/cli/polymesh mesh bench/geometries/public/plate.stl -h 0.05 --mesher graded
/usr/bin/time -f '%e s' ./build/apps/cli/polymesh mesh bench/geometries/public/plate.stl -h 0.05 --mesher hex
ctest --test-dir build -j$(nproc) --output-on-failure
```

(Adjust CLI flags to whatever `polymesh` actually exposes; check `apps/cli/main.cpp`.)

---

## Verification

### Per-node

1. `cmake --build build -j$(nproc)`  
2. `ctest --test-dir build -j$(nproc) --output-on-failure`  
3. Targeted: `ctest --test-dir build -R 'graded|mixed|conform|hybrid|octa|snap' --output-on-failure`  
4. Scoreboard row for public plate/cylinder when mesher code changes  

### Epic exit (E1)

- [ ] Graded face-conformity test green  
- [ ] Hybrid patch test green with **isoparametric** bulk hex  
- [ ] On `plate`/`cylinder_prism` at equal user h: hybrid and graded mesh time ≤ 1.5× hex  
- [ ] Graded no longer produces multi-million elements at GUI defaults for moderate models  
- [ ] Visual: hole silhouette hybrid/graded competitive with hex (quad-ish skin or successful snap)  
- [ ] Octa experimental path smokes  
- [ ] Docs: plan committed, ADRs amended, progress + ROADMAP updated  
- [ ] `graphify update .` if structural; commit graph artifacts  

### Anti-cheat

- No loosening GATE-1 analytical tols  
- No deleting conformity tests to go green  
- No hardcoded reference solutions in product code  
- Scoreboard numbers measured, not hand-waved  

---

## Parallel subagent playbook (handoff)

When driving this epic autonomously:

```
1. Land M0 + G0 + H0 + S0 in parallel (4 agents / worktrees if available).
2. Barrier: merge to master.
3. Land G1 and H1 in parallel (different files).
4. Barrier.
5. Land G2 + H2 (+ O1 anytime after M0b enum).
6. V1 then E1.
```

Each subagent prompt must include:

- This plan path  
- Exact node ID + acceptance  
- File ownership lane  
- “Commit+push master when green; no force-push; Hunter-124 author rules per CONTRIBUTING”  
- “Do not expand scope past node”  

Stuck protocol: 3 failed attempts → document in `docs/progress.md` Open issues, leave node red, continue sibling lane.

---

## Out of scope (explicit)

- True constrained Delaunay / Gmsh replacement (still ADR-0015 follow-on)  
- GPU meshing  
- Nonlinear FE  
- Replacing Eigen globally  
- Theme GATE A9  

---

## First concrete commits after approval

1. Write `docs/plans/mesher-solver-overhaul.md` (this plan) + ROADMAP track **H (Mesher honesty/perf)** with node table.  
2. **G0** conformity test that **fails on current graded** (documents bug) then **G1** fix in same or next commit.  
3. **H0** stamp port to mixed_fill (quick win users feel).  
4. **S0** BVH snap.  
5. Continue DAG.

---

## Risk register

| Risk | Mitigation |
|------|------------|
| 64-case transition templates incomplete | Fallback full-fine; LEB oracle test |
| Pyramid–Kuhn diagonal mismatch fails patch | Shared diagonal helper + patch test first |
| Scene.cpp merge conflicts | M0b enum-only commit; thin call sites |
| BVH numerical drift in snap | Bitwise-identical path optional; residual tolerance tests |
| Octa VEM unstable | Tet-split octa fallback |
| Scope explosion | Node ownership + “one node per agent session” |

---

## Summary

The hybrid and graded paths are not merely “unoptimized” — graded is **nonconforming at 2:1 interfaces**, and hybrid **throws away hex FE** in assembly while spending more DOFs on Kuhn skin. Fixing those two structural issues (G1, H2), plus shared snap/stamp perf (S0, H0), is the path to beating hex. Octahedral is a side experiment; solver AMG/ILU is a follow-on once mesh DOFs are honest.
