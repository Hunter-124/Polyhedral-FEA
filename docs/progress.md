# PROGRESS

## Active (read this first)

**Canonical plan:** [`docs/plans/advisor-measure-first-program.md`](plans/advisor-measure-first-program.md)  
**ADRs:** [0023](decisions/0023-measure-first-tet-primary-cvt-path.md) ·
[0024](decisions/0024-advisor-measure-answers.md)  
**Board:** [`docs/dag/PROGRAM.yaml`](dag/PROGRAM.yaml) ·
**Bootstrap:** [`docs/dag/AGENT_BOOTSTRAP.md`](dag/AGENT_BOOTSTRAP.md)  
**Roadmap:** [`docs/ROADMAP.md`](ROADMAP.md) · **Agent loop:**
[`docs/process/agent-loop.md`](process/agent-loop.md)

| Status | Nodes | Notes |
|--------|-------|--------|
| **Done** | **M0–M14 (all), G0–G4, V6e, V10c** | Measure path + full Geogram CVT stack (vendor→Lloyd→sites→export) |
| **In progress** | **M5** | Domain clip + bnd snap; **cylinder health+load_area OK**, SE loses to hybrid; plate load_area still FAIL (hole/non-convex) — `GATE.md` |
| **Open** | V6d, V11, campaign-1, feedback-loop | After health_ok / campaigns |
| **Deferred** | icecream face-tags | Face-ID BC design |

Order locked (ADR-0024 Q2): **freeze (done) → wall project (done) → CVT**. Dual
hard-block until G4. Packing “win” loops measure **delta vs M9 freeze only**.
Never score raw nodal max stress.

**Handoff:** [`docs/plans/SESSION_HANDOFF_g1-cvt.md`](plans/SESSION_HANDOFF_g1-cvt.md).

**Post-M10 smoke (2026-07-13):** cylinder+plate_hole × varyhedron+hybrid_zoo
h_scale=5 → **4/4 `ok`**, `health_ok` + `load_area_ok` true; cylinder SE ~0.0034
vs truth 0.00393. GUI `polymesh-gui` builds with health/scorecard/load_area Results panel.

## Background / older phases

**Track H (historical):** mesher honesty/perf overhaul; owner gate **A9** theme
polish. Plan: [`docs/plans/mesher-solver-overhaul.md`](plans/mesher-solver-overhaul.md).

**Windows (2026-07-10):** Release + GUI builds with MSVC 19.51 / VS 18 + vcpkg
(`eigen3`, `nlohmann-json`, `glad`). CLI mesh + GUI launch smoke OK. Root
binaries: `polymesh.exe`, `polymesh-gui.exe`. Full ctest not fully signed off
this pass (earlier Unicode Catch names + VTU temp locks fixed; suite can re-run).

GATE 1 deliverables ready:
- Full Tier-0 + Tier-1 suite (Lamé, Timoshenko, Kirsch, Goodier, L-domain)
- MMS convergence orders matching theory
- Gmsh `.msh` v2.2 ASCII import
- Convergence report: `bench/reports/p1-gate1-convergence.md`
- ADR-0009 (Tier-1 verification setups)

GATE 0 was approved by owner on 2026-07-09.

## Done
- 2026-07-13: **G1–G4 done (CVT critical path)** —
  - G1: Geogram Delaunay+Predicates PSMs, `clip_convex_cell`, unit-cube smoke
  - G2: `lloyd_cvt` + ρ=1/h³ (`SizeFieldFn` same contract as N_pred)
  - G3: sharp-fixed sites + OCC wall project (`constrained_lloyd_cvt`)
  - G4: `export_clipped_voronoi` → `PolyMesh` kPolyhedron (dual hard-block lifted)
  Catch2 `[geogram]/`/`[cvt]`/`[g3]`/`[g4]` green. Next: **M5** VEM gate.
- 2026-07-13: **M12 + M13 closed** — sphere polar cap `expected_area=π×10⁻³`
  (`z_p=0.04`, `normal_min_dot=0.7`); plate/cylinder already guarded. Icecream
  multi-face omits area (15–20% box swing) → face-tag design only. M13: full
  Legendre series **cut**; frozen dual-mesher fine reference SE `4.60e-4` + tip
  `2.90e-7` in `bench/reference/sphere.json` + hand-calcs. GUI Results:
  `load_face_area` / area_fail chip + richer scorecard tooltip.
- 2026-07-12: **M10 wall tangential smooth + OCC surface project** —
  `geom::project_point_on_surface` + `ProjectResult{point,normal,distance}`
  (BRepExtrema face + GeomAPI UV/normal; nullopt stub without OCC). Shared
  `mesh::wall_tangential_project` post-pass: free-boundary nodes far from sharp
  edges get Laplacian/tangential smooth + BRep re-project, Jacobian-safe revert.
  Wired into `varyhedron_fill` / pipeline `kVaryhedron` when CadModel present;
  STL-only unchanged. Catch2 `test_wall_project.cpp` + project API tests
  (cylinder mean radial residual not worse).
- 2026-07-13: **Cylinder load_area fix** — traction-aligned face select (`|n·t̂|>0.7`,
  box z≥0.195); load/area/tip probe share filter; guard ±5%. hybrid+vary cylinder
  `status=ok`, SE ~0.0034 vs 0.00393 (~14%). M12 still open for sphere/icecream.
- 2026-07-13: **M11 + M14 + G0** — testlab wall-clock kills (`max_run_wall_s`
  tier defaults 900/900/2700, `max_pack_wall_s`, `over_budget_cause=wall_clock`);
  h_min feature flags + `n_features_below_h_min` on results.jsonl (no OCC
  defeaturing); ADR-0025 Geogram vendor note + `third_party/geogram/` placeholder
  (G1 after M10). interfaces.md updated.
- 2026-07-12: **GUI Test Lab measure-first readiness (Hunter-124)** — ResultRow parses health/scorecard/answers/`solve_suspect`; Results chips (ok/suspect/fail/budget); baseline-m9 badge; handoff open questions (V10c partial); Varyhedron tooltip (ADR-0023/24).
- 2026-07-13: **M9 baseline freeze** — campaign `bench/campaigns/varyhedron-baseline-m9/`
  (4 STEP × varyhedron+hybrid_zoo × 1 tier h_scale=5.0 = 8 runs; warehouse +
  analyze + HANDOFF). **ok_rate 75%** (6/8); both cylinder runs `solve_suspect`
  (load_area gate). Code SHA `dcb2baa`; metric schema `scorecard-m1-m8-v1`.
  Canonical note: [`BASELINE.md`](../bench/campaigns/varyhedron-baseline-m9/BASELINE.md).
  Packing deltas vs this freeze only; next **M10**.
- 2026-07-12: **M12 partial — expected_area on planar STEP loads** —
  `plate_hole` load `expected_area=0.001` (end face \(H\times t=0.1\times0.01\));
  cylinder tip \(\pi R^2\) already present. Sphere polar-cap + icecream multi-face
  deferred. Design stub `docs/research/brep-face-tag-bc.md` (Q7). M12 stays
  in_progress until those two cases get area or face-ID BCs.
- 2026-07-12: **M6–M8 measure substrate complete** — element-centroid face-mean VM +
  strain_energy scoring (drop raw nodal max); OCC κ + mesh-segment chordal e;
  protecting balls r=min(αh, β·lfs) + corner shrink. PROGRAM nodes done; next is M9 freeze.
- 2026-07-12: **Program board notes polished for agents** — every open M6–M14 /
  G0–G4 note cites ADR-0024 Q# + `docs/plans/advisor-measure-first-program.md`;
  V11 dual hard-block until G4 + no packing loops until M9; V6d keeps M1 dep +
  curved boundary before p>1.
- 2026-07-12: **Advisor plan fully documented for agents** — canonical
  `docs/plans/advisor-measure-first-program.md`; ADR-0024 full Q1–Q10;
  PROGRAM.yaml nodes M6–M14 + G0–G4; README / CLAUDE / CONTRIBUTING /
  AGENT_BOOTSTRAP / ROADMAP / dag README all point at the plan. Order locked:
  freeze baseline → wall project → Geogram/CVT; dual hard-blocked; VEM gated.
- 2026-07-12: **CadTopology sharp/smooth/seam edges** — `CadEdgeFeature` + dihedral
  classify in `extract_topology` (25° from flat); `edge_profile_hausdorff_filtered`,
  `chordal_edge_metrics`, `count_edge_features` for protect-only residual / chordal
  efficiency. Catch2: cube sharp, cylinder seam+sharp rims, plate_hole hole rims.
- 2026-07-12: **M1–M4 measure lane code** — face-mean probes + health/`solve_suspect`;
  scorecard (sharp Hausdorff/h, chordal e, normal dev); varyhedron protect/snap
  **sharp-only** (seams skipped); N_pred + `over_budget_cause`. Wall OCC project
  + h_min virtual-topology still open.
- 2026-07-12: **ADR-0023 measure-first pivot** — tet FE default product claim;
  poly VEM gated (M5); weighted restricted CVT ranked over dual-of-tet/frame
  fields; program Lane **M0–M5** (probes → scorecard → sharp-only protect →
  N_pred sizing → VEM gate); V6d/V6e/V11 rewired to depend on M1/M2. Do not
  run packing “improvement” loops until M1 health + face-mean probes land.
- 2026-07-12: **Lane V wave — 100% smoke + orphan compact** — `NodalMesh::compact_unused_nodes` fixes singular K on varyhedron cylinder; testlab face/padded Dirichlet + direct LDLT; V1c BRep Model, V3c GUI HEAD, V4 CAD auto-h, V6c packing seeds, V9b warehouse shots; smoke 4/4 ok; varyhedron-short-1 running.
- 2026-07-12: **Lane V docs/gates (V1d, V2d, V9b, V10d)** — product OCC docs
  (Ubuntu libocct + Fedora `opencascade-devel`); `check_no_product_stl.sh` +
  CI; `warehouse_shots.py` mesh.vtu→wire.png; grok invoke force-push deny
  confirmed.
- 2026-07-12: **Varyhedron Jacobian-safe edge snap + smoke campaign** — soft CAD edge blend with volume-offender revert; first warehouse smoke (4 runs) + HANDOFF pack; coarser short-campaign tiers (h_scale 5/3.5/2.5).
- 2026-07-12: **Varyhedron v1 path (V6a/V6b/V7)** — `VolumeMesher::kVaryhedron`,
  `mesh/varyhedron_fill` (CAD edge seeds + graded scaffold + edge-profile snap),
  GUI label/tooltip (ADR-0021), CLI/testlab `varyhedron`, smoke on
  `plate_hole.step` with `geom_source=brep_topology`. Catch2
  `test_varyhedron_fill`.
- 2026-07-12: **V3a warehouse layout + git-LFS** — `.gitattributes` tracks
  `*.vtu` and `bench/campaigns/**/runs/**/*.png` via git-LFS;
  `bench/campaigns/README.md` documents
  `runs/<cfg_id>/<part>/t<tier>/{mesh.vtu,wire.png,quality.json,result.json}`;
  skeleton `varyhedron-short-1` campaign (`warehouse` + `on_finish`
  analyze/grok_handoff; 4 shape placeholders × varyhedron+hybrid_zoo × 3
  tiers `keep_frac: 1.0`). Writer is V3b; run is V8.
- 2026-07-12: **Lane V program board (V0)** — ADRs 0020 (true BRep volume
  meshing), 0021 (varyhedron packing from day 1), 0022 (full experiment
  warehouse + headless `grok -p --yolo` loop); ADR-0001 amended (OCC product
  path, STL compare-only); `docs/dag/PROGRAM.yaml` V0–V11 nodes;
  interfaces §7–§8 warehouse/handoff; `docs/process/grok-loop.md`;
  AGENT_BOOTSTRAP open-node list. Owner shapes: plate_hole, cylinder, sphere,
  icecream_cone; meshers varyhedron + hybrid_zoo; ~3 runs/shape short campaigns.
- 2026-07-12: **plate_hole outer-corner mesh artifacts** — `write_plate_hole`
  ray-to-rect top/bottom faces chord-cut the four rectangle corners and left
  the STL non-manifold against full-side vertical walls; volume snap then
  produced fan/notch junk at the outer corners (hole itself was fine). Fix:
  insert true corners on wall transitions, segment outer walls to match the
  top/bottom polyline, manifold self-check in the generator. Regenerated
  `tests/fixtures/parts/plate_hole.stl` (400 tris, edge multiplicity 2).
  Hex snap max|d| ~0; hybrid corners sharp.
- 2026-07-11: **Feedback-loop analysis scaffolding** — `scripts/analyze_campaign.py`
  mines partial/finished `results.jsonl` → weighted ranking, accuracy-vs-time
  Pareto (global / part / geom_class), knob suggestions; writes
  `PARETO.md` + `PARETO.json`. Docs: `docs/process/feedback-loop.md`,
  interfaces §3b. Ran on smoke (finished, hex wins) and settings-frontier-1
  (still running tier 0; provisional hex-leaning tendency, no default code
  changes). PROGRAM: campaign-1 remains in_progress; feedback-loop todo with
  partial-analysis note.
- 2026-07-11: **Campaign-1 settings frontier started** — `bench/campaigns/settings-frontier-1`
  full factorial (meshers × feature_refine × element_tendency) on smoke_bar /
  plate_hole / cantilever with successive-halving tiers. Testlab wires
  hybrid_vem + element_tendency into volume_mesh.
- 2026-07-11: **GUI sim controls (DAG `gui-sim-controls`)** — interactive
  `SolveJob` exposes `JobProgress` (phase / phase_frac / elapsed_ms / adapt
  pass) matching interfaces.md §6 vocabulary; cooperative **pause / resume /
  cancel** between mesh·adapt·solve phases (not mid-CG). Sim Setup panel:
  live ProgressBar + elapsed, resource knobs (`max_threads` →
  `fea::set_openmp_threads`, `max_mem_gb` soft note). Test Lab: play/run +
  SIGINT pause + force stop; campaign/GUI thread caps set `OMP_NUM_THREADS`
  for harness children. Gates: `test_gui_pipeline` progress/cancel/pause,
  `test_backend` thread cap restore.
- 2026-07-11: **Test lab harness + GUI Test Lab (DAG `testlab-harness`, `gui-testlab`)** —
  `apps/testlab/polymesh_testlab` campaign runner (successive-halving, SIGINT
  checkpoint, results.jsonl, progress.json; anti-cheat reference load). Smoke
  campaign green on smoke_bar (hex rel_err≈3.7%, hybrid_zoo ≈14.8%). GUI:
  Test Lab | Sim Setup | viewport | Results; ImGui-free parsers + ProcessRunner.
- 2026-07-11: **Joint (h,p,shape) adaptive driver (DAG `hp-driver`, ADR-0019
  §4)** — `adapt/hp_driver.{hpp,cpp}`: per-element utilities from geometry
  turning angle / thin-wall (a priori → h), ZZ + hierarchical surplus
  smoothness (→ p), and shape fitness + DOF cost heuristics (→ shape
  tendency). `drive_hp` emits h/p/shape marks, seeded `AdaptSuggestion`, and
  global mesher tendency. `SolveJob` adapt loop builds signals from ZZ η +
  surface κ/thickness, applies mid-loop p when p dominates, and can flip
  `kHybrid`→`kHybridVem` / tet↔hex on majority shape vote. Gates:
  `tests/test_hp_driver.cpp` (curved→h, smooth→p, shape flip, deterministic).
  Cost weights are v1 heuristics; campaign calibration is `feedback-loop`.
- 2026-07-11: **Element-tendency dial (DAG `mesher-tendency`)** —
  continuous `element_tendency ∈ [-1,+1]` on `SimSetup` and `volume_mesh`
  (`resolve_element_tendency`). Hybrid-family map: hex fill / fan-split
  `kHybrid` / native-poly `kHybridVem` / graded tet; t=0 preserves base;
  mild hex bias thins skin. CLI `--element-tendency`. Gate:
  `tests/test_element_tendency.cpp` (resolve thresholds + cell-kind mix on
  unit box with seed transitions). Campaign grid key is live.
- 2026-07-11: **Hierarchical p≥3 (node `p-hierarchical-highp`)** —
  multi-mode entity DOFs; tet edge sign (−1)^m on reversed edges; hex
  quad-face dihedral transform; tet k≥3 face/interior kernels (p≤4); hex
  p≤6; Gauss n=6. MMS energy rates p=1..4: **1.02 / 1.99 / 2.98 / 3.98**.
  Q2-poly exact at p=2,3,4. Full suite 161 green.
- 2026-07-11: **Unified mixed FE+VEM assembly (DAG `fe-vem-assembly`, ADR-0019
  §1)** — hybrid zoo gains `native_poly_transitions`: each 2:1 transition
  coarse cell is one unsplit polyhedron (`MixedCellKind::kPolyVem`) with faces
  matched to bulk FE hex / fine 2×2×2 hex (no centroid apex, no fan slivers).
  `VolumeMesher::kHybridVem` (CLI `hybridvem`, GUI "hybrid VEM") keeps hex as
  FE and solves poly cells as VEM in the **same** `assemble_stiffness` K.
  Gate: constant-strain patch (`u=Gx` on boundary) exact to 1e-9 across
  FE/VEM interfaces — `tests/test_fe_vem_assembly.cpp` (checkerboard, tet+hex
  VEM, native-poly fill, pipeline path). Docs: solver-core §3 expanded;
  PROGRAM.yaml node `done`. Default `kHybrid` product-FE path unchanged.
- 2026-07-11: **Validation part library (DAG node `part-library`)** — three
  solid fixtures under `tests/fixtures/parts/` for the test lab, each with
  `.stl` + `.case.json` + `bench/reference/<name>.json` (schemas in
  `docs/dag/interfaces.md` §4–§5) and closed-form derivations in
  `docs/validation/hand-calcs.md`. **smoke_bar** (0.1×0.01×0.01 m, E=2e11,
  ν=0.3, clamp x≈0, end traction 1e6 Pa): σ_vm=1e6 Pa, tip ux=5e-7 m.
  **plate_hole** (Kirsch plate, a=0.01 m, remote tension 1e6 Pa): SCF=3.0.
  **cantilever** (1.0×0.1×0.1 m, tip traction −1e5 Pa → P=1 kN): Timoshenko
  tip deflection 2.0153e-4 m. Geometry regenerator
  `scripts/gen_part_library.py` (does not emit truths — anti-cheat). Node
  marked `done` in `docs/dag/PROGRAM.yaml`.
- 2026-07-10: **Conforming hierarchical assembly + MMS proof (ADR-0019 lane B)** —
  `fea/hp_assembly.{hpp,cpp}`: per-entity global DOF numbering (vertices,
  edges, faces, cell interiors) with the **minimum rule** for mixed order,
  conforming assembly, consistent body load, partitioned Dirichlet solve, and
  energy-norm error. At order ≤2 every orientation sign is +1 (φ₂ even, hex
  face mode symmetric), so no sign bookkeeping is needed yet. Tests
  (`test_hp_assembly.cpp`): a **mixed p1/p2** constant-strain patch reproduces
  a linear field to **0 error across the order interface** (the min rule keeps
  it conforming), and an MMS problem (u=sin πx·sin πy·sin πz, homogeneous
  Dirichlet) converges in the energy norm at **rate 1.00 (p=1)** and
  **2.00 (p=2)** — the end-to-end proof that shared entity DOFs assemble
  correctly. Full suite 157 cases green. Follow-on: p≥3 orientation
  signs/transforms + tet k≥3 kernels (node `p-hierarchical-highp`).
- 2026-07-10: **Hierarchical arbitrary-p basis foundation (ADR-0019 lane B)** —
  `fea/hierarchical.{hpp,cpp}`: 1D integrated-Legendre (Lobatto) basis with
  derivatives (vertex funcs + order-k bubbles φ_k = (P_k−P_{k−2})/√(2(2k−1))),
  hex full tensor-product hierarchical modes at order 1..4, tet vertex +
  quadratic edge bubbles at order 1..2, subparametric single-element
  stiffness. Modes carry entity/order/orientation descriptors (`HpMode`) for
  the forthcoming per-entity DOF assembler. Tests (`test_hierarchical.cpp`,
  6 cases): Lobatto endpoint/derivative identities, p=1 stiffness ==
  frozen nodal hex8/tet4, exactly six rigid-body modes at every order on
  distorted geometry, SPD. Full suite 155 cases green. Next: per-entity DOF
  numbering + orientation signs + MMS h/p-convergence (node `p-hierarchical`).
- 2026-07-10: **Adaptive-core program bootstrapped** — repo-tracked DAG
  (`docs/dag/PROGRAM.yaml` + interfaces.md + README) as the pick-up-anywhere
  board; ADR-0019 (mixed FE+VEM, arbitrary-p hierarchical, min-rule
  conforming, (h,p,shape) driver); CONTRIBUTING §0 AI-agent contributor quick
  start. Test-lab harness, validation part library, and GUI panel rebuild
  under way in parallel (DAG lane A).
- 2026-07-10: **Curvature-driven refinement + boundary finishing (bore/rim
  weirdness fixed)** — percentile curvature seed balls replaced by a per-cell
  turning-angle criterion (`stamp_curvature_cells`: refine where the surface
  turns > 15° per cell, h·κ > θ; L2 at > 2θ) in both graded tet and hybrid
  zoo — inert on flats (no more fine islands), contiguous around bores (no
  more coarse rings). Hybrid v4 latent fan-anchor bug fixed (corner anchor →
  7399 zero-volume tets at hole-fine h; now anchors at min-id mid node, raw
  min aspect 0 → 0.125). Free-surface transition cells promoted to fine so
  fan tets never sit on the wall (hole-fine hybrid M1max 0.0876 → ~1e-11,
  M6 0 → 0.17). New S6 crease-aware tangential boundary smoothing
  (`mesh/surface_project::smooth_boundary_nodes`, offender-revert guard,
  intrinsic normal-cone crease freeze) kills the sawtooth rim at hole edges;
  degenerate flat caps peeled in both meshers. Scorecard (hex/graded/hybrid):
  sphere 0.849/0.804/0.896, cylinder 0.860/0.792/0.861, hole
  0.568/0.530/0.577; hole-fine hybrid 0.424 > hex 0.410. All 149 tests pass.
  ADR-0012 amended.
- 2026-07-10: **Hybrid v4 conforming fan transitions + graded S4/S5 repair
  (curved scorecard flipped to pass)** — root causes found & fixed: hybrid v3
  2:1 pyramid transitions were non-conforming (hanging edge-mids → cracked
  meshes, exposed interior apex faces; sphere score 0.46 vs hex 0.85) → v4
  polygon-fan closure (mid exists iff an incident cell is fine; canonical
  min-id fan pairs both sides; `mixed_fill.cpp`). Graded snap left degenerate
  boundary caps (min aspect ~1e-18) and hole-void jut nodes (~0.25 h) → S4
  conforming cap collapse + S5 jut-star void carve + second snap round
  (`hybrid_fill.cpp`); hybrid scene snap gained per-node outlier re-projection
  with partial fractions. Measured (equal h): sphere hex 0.849 / graded 0.799 /
  hybrid 0.896; cylinder 0.860/0.780/0.860; hole 0.568/0.530/0.577; graded &
  hybrid M1max ≈ 0. `test_curved_mesh_quality.cpp` inverted from DOCUMENT_BUG
  ceilings to pass floors + residual/aspect hygiene. ADR-0012 amended.
- 2026-07-10: **Curved mesh scorecard + graded free-surface fixes (T0/Q1–Q2)** —
  New `mesh/surface_metrics` (M1 node residual, M2 face-sample residual, M3 volume
  error, M4 radial, M5 azimuth gap, M6 boundary aspect + composite). Catch2
  `test_curved_mesh_quality` on sphere / cylinder_prism / `test.stl` hole plate:
  hex must pass floor; graded/hybrid documented under bug ceilings and lag hex
  (flip assertions after residual wins). Fixes: LEB free-edge midpoints project
  onto STL with Jacobian chord fallback (`local_refine` + surface arg from graded
  fill); post-LEB snap uses only unpaired-face nodes; unsnap line-search
  0.75→0.5→0.25; graded curvature seeds spatial-thinned like hybrid (0.75h /
  cap 256). Related mesher suite green. Remaining: flip scorecard to pass bars
  when graded residual on hole plate beats hex competitiveness; hybrid free-
  surface size consistency (S4); graded perf after quality.
- 2026-07-10: **Hybrid zoo v3 true size adaptivity (hole transition usable)** —
  Root cause of “no adaptive size”: hybrid only swapped hex↔pyramid at fixed `h`.
  Fix: **2:1 fine** (2×2×2 hex @ h/2) on feature/seed cells + **pyramid transition
  cells** on interior coarse neighbors (no hanging faces). Free-surface never hosts
  transitions (gap-close 2 hops only — long FS BFS flooded flat faces). Spatial
  seed thinning (min sep 0.75h, cap 256) so hole wall is refined **all around**
  (index-order 192-seed cap had clustered one sector). Post-expand surface snap
  (pyramid Jacobian) → snap max|d|≈0. Graded tet unchanged multi-level LEB path.
  **Scoreboard (`tests/fixtures/test.stl`, auto+feature):** hybrid ~280k pyr /
  ~3.7 s, h_bulk=1.59/h_fine=0.79, fine_cells=3399 transition=2856 feature=1876,
  curv_seeds=168, snap≈0, azimuthal short-edge coverage uniform; graded ~153k
  tet / ~3.2 s, snap mean|d|≈0.008 max 0.645 (ADR-0015). Shots:
  `bench/mesher/shots/test_{hybrid,graded}{,_hole,_hole_iso,_hole_top}.png`.
  Catch2 hybrid/mixed/graded green (incl. 2:1 size test).
- 2026-07-10: **Hole transition + adaptive size (verified on `test.stl`)** —
  Snap: smarter feature prefer (rim only, not hole wall), soft-then-full unsnap,
  pre-LEB + post-LEB + accept/reject residual reproject. Auto-h: Rκ/6 (~6 bulk
  cells across hole radius). Feature/seed bands widened (2h / 1.6h); L2 feature
  core 0.75×band. **Free-surface skin flood OFF when feature/seed grading is on**
  so L0 bulk vs L1/L2 hole contrast is visible (was flooding whole exterior →
  uniform look). Hole-zoom harness: `scripts/vtu_wire_png.py --hole-zoom`.
  Residual graded max|d| still ~0.4h on few unsnapped Kuhn nodes (ADR-0015).
- 2026-07-10: **Adaptive size + surface quality (mesher product fix)** —
  Multi-level graded LEB (L0 bulk / L1 feature / L2 high-κ → ~h, h/2, h/4);
  thin plates skip free-surface flood when feature grading is on (size contrast);
  **post-LEB exterior recollect + re-snap** (mid-edge hole nodes no longer miss
  snap); edge-aware snap prefers sharp CAD creases; auto-h no longer densifies
  from STL facet count (uses Rκ/thickness; dens floor 0.88). Hybrid: same thin
  + edge snap; octa cell budget. Hole-plate (`test.stl`) auto: graded ~69k /
  1.3s, hybrid ~36k / 0.6s (was multi-million / unusable). Harness:
  `scripts/mesh_preview.py` (90s timeout) + `scripts/vtu_wire_png.py` →
  `bench/mesher/shots/`. Catch2 graded multi-level size ratio + conformity green.
- 2026-07-10: **Graded tet interactive again (LEB perf)** — `local_refine_tets`
  was O(n²) (full-mesh edge scan + rebuild every bisection); edge→tet adjacency
  + in-place child replace. Graded fill uses **one** LEB pass for true 2:1
  (second pass re-marked the same cells → ~4:1 and multi-minute freezes). Auto-h
  unit_box graded+feature: ~70 s → **~0.5 s**; public STLs graded ≈ hybrid.
  Catch2 local_refine + graded + conformity green. Root `polymesh*.exe` rebuilt.
- 2026-07-10: **Mesher overhaul wave 2 (WIP handoff)** — H2: hybrid zoo → hex
  bulk + pyramid skin, product FE expands hex→pyramids (removed Kuhn-hex
  assembly); O1: experimental `octa_fill` + `VolumeMesher::kOctahedral` +
  CLI/GUI; V1: CG IncompleteLUT with diagonal fallback. Builds; **full ctest
  not verified this commit** — run suite on next machine before claiming green.
- 2026-07-10: **Mesher overhaul wave 1 (Track H)** — Plan on disk
  (`docs/plans/mesher-solver-overhaul.md`); ADR-0018 graded LEB conformity
  (no 2:1 hanging Kuhn); `tet4_face_conformity` + Catch2; shared
  `cell_stamp` in hybrid zoo; surface grid-hash closest-point; hybrid
  thinner feature/seed defaults; mesher scoreboard script. **141** tests green.
- 2026-07-10: **Graded tet coarse-primary lattice** — Recovered WIP after agent
  crash: classify at target \(h\) (same cost class as tet/hybrid), then local
  \(2×2×2\) Kuhn only on skin/feature/seed cells (bulk≈\(h\), fine≈\(h/2\)).
  Replaces fine-global lattice + coarse-block aggregation. Boundary quads
  emitted per exterior coarse/fine face. **138/138** Catch2 green on related +
  full suite.
- 2026-07-10: **Graphify shared workflow** — Rebuilt `graphify-out/` (AST +
  docs); gitignore machine-local artifacts; CONTRIBUTING §8 + `CLAUDE.md`
  document clone setup, `graphify update`, hooks, merge driver for concurrent
  graph.json updates.
- 2026-07-10: **Graded tet fix (size + speed + RAM)** — Dropped global \(h/4\)
  lattice when features/seeds active (was bulk only \(h/2\), 8× cells, thin plates
  fully fine → slow mesh + FEA OOM). Always **2:1** (bulk≈\(h\), fine≈\(h/2\));
  feature/seed stamp via rasterized balls (not O(blocks·seeds)); skin depth
  capped by interior thickness; snap Jacobian only on boundary-touching tets;
  pipeline seeds sparse (≤192, 85th-κ, band≈1.25\(h\)); p-elevate skipped when
  nodes>40k. GUI: skin=2, p-elev opt-in. Tests updated (subdiv always 2).
- 2026-07-10: **Performance build** — Release defaults to **-O3**; OpenMP ON for assembly, mesh classify (uint8 mask, not vector<bool>), ZZ, stress, SpMV; Eigen kept serial to avoid nested OpenMP hangs; no -ffast-math; LTO/native-arch OFF (Eigen miscompile risk). `polymesh backend` reports thread stack. 133 tests green.
- 2026-07-10: **Results viewport + geo-hybrid mesh** — pan/orbit fixed in von
  Mises/deflection/error (Image hover captured before colorbar child);
  auto-exaggerated deformation (max |u| → ~12% of model diagonal, true-scale
  checkbox); graded fill targets **h/4** near feature/seed bands (subdiv=4)
  so curved edges densify vs bulk h/2; more aggressive κ/thin seeds + thicker
  skin default; pre-solve **geo-hp** bulk p-elevate (tet10 interior, linear
  near surface); GUI defaults graded+feature+adapt+p-elev. 132 tests green.
- 2026-07-10: **GUI layout + mesher product pass** — group-box right padding
  (content child reserves both sides); single workspace tiles study|splitter|
  viewport (no purple gap); fixtures: CAD face list + click-to-select without
  orbit fight + “show CAD” when in mesh mode; mesh preview checkerboard + dark
  wireframe with depth bias; multi-pass surface snap ≤0.55h on tet/graded/
  hexpyr; graded feature path seeds curvature (cylinder/hole) + thin-wall
  bands; 131 tests green.
- 2026-07-10: **Graded tet “grid too fine” fix + full-adapt product path** —
  `make_bbox_grid` / `make_bbox_grid_even` auto-coarsen under the 512k cell
  budget (no hard fail); graded fill pre-floors \(h\) for the fine \(h/2\)
  lattice; adapt loop uses multi-wave LEB, grid-aware \(h\) floor, graded seed
  remesh; GUI defaults graded tet + 3 adapt passes + η=0.12. Catch2 tiny-h
  graded + grid budget tests.
- 2026-07-10: **Mesh gap fix** — shared-edge ray-parity double-count punched
  diagonal tunnels through cubes/plates (cells with \(c_x\approx c_y\) outside);
  bbox-fitted anisotropic lattice so nodes hit AABB faces. Shared
  `mesh/grid_classify` used by tet/hex/graded/transition/prism. Unit box volume
  exact (6000 tet @ h=0.1); edge fixtures thin plate / slender / offset / sphere;
  Catch2 regressions. ADR-0015 updated.
- 2026-07-10: Fix GUI mesh-only freeze — stop corner geometry-sizing from shrinking global h 8×; O(n) element-type colors in viewport; live meshing status; `build.bat`/`build.sh` copy CLI+GUI to repo root.
- 2026-07-10: D6 Tier-3 instrument — L-domain uniform tet10 vs geometric graded
  tet10 (same solver, ADR-0005). Harness: `apps/bench/polymesh-d6-tier3` +
  `bench/d6/run_tier3.py`; raw `bench/d6/out/…-raw.json`, scoreboard rows
  `bench/results/polymesh-d6-l-domain.json`; writeup `docs/bench/d6-tier3.md`;
  label `d6-tier3`. Measured (full suite): **5.12× DOF** and **12.2× wall time**
  at matched strain energy (graded `h0=w/8_rho2` 1248 free DOFs / 0.23 s vs
  uniform n6 6384 DOFs / 2.76 s; energy match tol 0.01%). Catch2 smoke for
  script --help / JSON schema (not multi-minute bench). ROADMAP D6 closed on
  this instrument; product-mesh Tier-3 on full public geometry suite still open.
- 2026-07-10: F3 CUDA SpMV scaffolding — `fea/spmv.hpp` CSR + `spmv_cpu` (always),
  `try_spmv_cuda` / device kernel in `backend_cuda.cu` when `POLYMESH_WITH_CUDA=ON`,
  Catch2 CPU vs Eigen + CUDA-vs-CPU parity (SKIP without toolkit/device). Default
  CI remains CPU-only. README CUDA enable notes. ROADMAP F3 closed.
- 2026-07-10: C3 prism sweep volume fill — `prism_fill_surface` (Cartesian
  lattice, each inside voxel → 2× prism6 along longest bbox axis); pipeline
  `VolumeMesher::kPrismSweep`; CLI `--mesher prism|sweep`; GUI mesher combo;
  Catch2 validity + constant-strain patch + solve smoke; ADR-0015 updated.
  Honesty: not CAD extrusion detection (same grid-fill limits as tet/hex).
- 2026-07-10: C4 VEM k=2 — serendipity edge midpoints on `kPolyVem` (order
  inferred: nv→k=1, nv+ne→k=2); hex path = isoparametric hex20 (ADR-0017);
  patch test + degree-2 exact + MMS energy-norm order ≈2 ±0.2; k=1 unchanged.
- 2026-07-10: D4 true local h-refine — ADR-0016 Rivara longest-edge bisection
  (LEPP, no hanging nodes); `mesh::local_refine_tets`; Catch2 single-tet +
  multi-tet center mark (validity, +volume, volume conserve) + solve smoke;
  pipeline adapt tries LEB on tet/graded-tet before seed remesh (ADR-0014).
- 2026-07-10: D3 p-elevation — `fea::promote_to_quadratic` / `fea::p_elevate`
  (tet4→tet10, hex8→hex20, shared mid-edge map); `adapt::mark_smooth` (Dörfler
  complement); `SimSetup::p_elevate` + auto when `adapt_passes>0`; CLI
  `--p-elevate`; GUI checkbox; Catch2 promote/patch/selective/mark tests.
  test_support wraps product API.
- 2026-07-10: C5 Kirsch equal-DOF graded vs uniform tet — structured annular
  tet10 (ADR-0009 BC setup; not Cartesian product fill — stair-case on hole,
  ADR-0015). Same free DOFs (648); log radial map vs linear: SCF rel err
  **0.70%** vs **3.06%** (analytical SCF=3). Catch2
  `test_kirsch_c5_graded.cpp`. ROADMAP C5 closed (GATE 3 Kirsch leg).
- 2026-07-10: C2 curvature + thin-wall indicators — `geom::estimate_vertex_curvature`
  (dihedral 1-ring |H| proxy) + `estimate_local_thickness` (inward ray cast);
  `adapt::GeometrySizing` / `make_geometry_sizing` mins sharp-edge blend, h≈c/κ,
  h≈f·thickness; pipeline feature-grading samples geometry sizing. Catch2 thin
  plate vs bulk + sphere vs flat. ROADMAP C2 closed.
- 2026-07-10: F2 iterative CG solve — `SolveOptions` / `SolveMethod`
  (`kAuto`|`kDirect`|`kCG`); default auto switches to Eigen
  `ConjugateGradient` + `DiagonalPreconditioner` when free DOFs > 8000
  (else `SimplicialLDLT`). `select_solve_method` for diagnostics. Catch2:
  forced-CG vs direct cantilever + patch, auto CG on ~15k free-DOF hex
  cantilever. README + `solve.hpp` docs. Patch-test direct path unchanged.
- 2026-07-10: G2+G3+G4 — `examples/` README + `run_mesh_public.sh` /
  `run_solve_public.sh` (auto-h CLI on `bench/geometries/public/*.stl`, symlink
  geometries); public-header SI units/doxygen spot-check (SimSetup, volume_mesh,
  write_vtu, sizing, tet/hex/graded/transition fills); CI `actions/checkout@v5`
  (setup-python stays @v5); ROADMAP G2–G4 closed.
- 2026-07-10: F1 OpenMP assembly — CMake `POLYMESH_WITH_OPENMP` (default ON)
  finds OpenMP; when present, `fea` links `OpenMP::OpenMP_CXX` and defines
  `POLYMESH_WITH_OPENMP`. `assemble_stiffness` uses `#pragma omp parallel for`
  with thread-local triplets (critical-free hot loop), then merges; serial if
  OpenMP missing. README notes. Patch/Tier-0 remain green with OpenMP on.
- 2026-07-10: E1/E2/E3 — CalculiX peer `run_calculix_cantilever.py` (skip exit 0
  without ccx; JSON when present); gate1-p1 Lamé/Kirsch/cantilever scoreboard
  + `emit_polymesh_gate1.py` best-effort DOF fill; `audits/README.md` holdout
  protocol (no secret geometries). Scoreboard regenerated.
- 2026-07-10: B1/B3/B4 — ADR-0015 Cartesian grid-fill limits (not Delaunay);
  surface-snap Jacobian safety (unsnap nodes that invert tet / hex J / pyramid
  volume); README OCC enablement (Ubuntu libocct-* + `POLYMESH_WITH_OCC=ON`);
  Catch2 unit-box snap + L-domain fixture validity. B1 = documented limits.
- 2026-07-10: D5+E4+G1 — `resolve_mesh_size` (bbox extent/16 ∩ diagonal/28 +
  sharp-edge density / min feature); pipeline mesh-only+solve and CLI omit `-h`
  use it; mesher_note carries `auto h=…` for GUI. E4 Catch2 product-mesh box
  cantilever (max|u|>0, finite σ_vm) + cylinder_prism smoke (not Lamé tol).
  README quickstart: Ubuntu apt (CI list), cmake/build/ctest, CLI mesh/solve on
  `unit_box.stl`, GUI argv/auto-h note. 81 tests (with C1).
- 2026-07-10: C1 hybrid honesty — product FE path `expand_hex_core_to_pyramids`
  (interior hex → 6 pyramids, matching face diagonals); pipeline kHexPyramid
  always expands; Catch2 hybrid constant-strain patch < 1e-12 on mixed lattice;
  ADR-0013 amended. Pure-pyramid patch unchanged. 77 tests.
- 2026-07-10: B2+B5 — VTU `VtuCellData` + tet4 `quality` cell array on CLI/GUI export;
  Catch2 CellData XML check; public fixtures `l_domain`/`plate`/`cylinder_prism` +
  README; STL load smoke. 74 tests.
- 2026-07-10: GUI A6/A7/A8 — wireframe + undeformed outline toggles (OpenGL
  boundary edges), GLFW drag-drop open (.stl/.step/.stp) with path field
  fallback, mesh_note + DOF (3×nnodes) in sidebar/status after mesh/solve.
- 2026-07-10: D2 global η stopping criterion — `SimSetup::eta_target` (0=off);
  adapt loop early-stops when `global_eta ≤ eta_target`; CLI `--eta-target`;
  GUI η input near adapt passes; Catch2 early-stop + disabled-path tests.
- 2026-07-10: CI green again — clang-format 18.1.8 pinned in workflow (was drift vs local), full tree reformat; rename `namespace pipe` alias in test_transition_fill (POSIX `pipe()` collision on Ubuntu).
- 2026-07-10: Master ROADMAP + agent-loop protocol; GUI M1 path — argv open,
  mesh-only job + element-type preview, ZZ error field + colorbar, failure
  dismiss, public `unit_box.stl` fixture. (in progress / this commit)
- 2026-07-10: A posteriori adapt seeds — Dörfler centroids → graded fine balls;
  `suggest_refine`; pipeline adapt remesh; CLI `solve --adapt n`. ~70 tests.
- 2026-07-10: Graded tet feature band (sharp-edge distance), pipeline
  `feature_refine`, CLI `--feature`, feature-block stats in mesher notes. 68 tests.
- 2026-07-10: FeatureSizing field + feature-aware solve h; pyramid5 patch test
  (pure pyramid lattice); pyramid base orientation for +Jacobian; Duffy product
  quadrature; documented hex–pyramid hybrid nonconformity (ADR-0013). 66 tests.
- 2026-07-10: Hex+pyramid boundary snap (0.35h), pipeline residual distance note,
  CLI solve/mesh `--mesher` + `--skin`, pyramid tet-split stiffness (flip-safe
  scatter), ADR-0013 snap notes. 63 tests.
- 2026-07-10: Graded tet fill (fine skin / coarse core), surface conformity
  metrics, ADR-0012 (hybrid = graded all-tet until pyramids). 58 tests.
- 2026-07-10: Prism6 wedges; hex-VEM hybrid; quality metrics; CalculiX smoke.
- 2026-07-10: Hex grid fill option + GUI mesher selector; tet quality notes.
- 2026-07-10: VEM k=1 polyhedra (patch test + 6 RBM), adapt_passes in pipeline,
  feature grading, CalculiX smoke peer, GUI adapt/feature controls. 50/50 tests.
- 2026-07-10: Product batch — VTU export, ZZ recovery + Dörfler marking,
  sharp-edge features + graded sizing, limited surface snap on tet fill,
  CLI `mesh`/`solve`, GUI STEP paths + theme switch + VTU export button,
  linguist fix (graphify HTML vendored). 47/47 tests green.
- 2026-07-10: Optional OpenCASCADE STEP path — `geom::load_step`, CMake
  `POLYMESH_WITH_OCC` finds OCCT (TKDESTEP + BRepMesh), stub throws when OFF;
  Catch2 tests + unit-cube fixture.
- 2026-07-10: G1 — ADR-0010 keep face-based mesh; geometric validity;
  `mesh::tet_fill_surface` (tet4 grid fill); pipeline/GUI use tet4 path
  (replaces draft voxel hex8). 39/39 tests green.
- 2026-07-10: Campaign G0 — branch `master`, BSD-3-Clause, apps/src split,
  pipeline vs GUI separation, CONTRIBUTING/CHANGES, docs under docs/.
- 2026-07-09: D1–D5 + GUI scope ratified with owner (ADR-0001..0006).
- 2026-07-09: License BSD-3-Clause applied; process docs live under docs/.
- 2026-07-09: Owner switched language to C++ (ADR-0007) and made CUDA a
  first-class optional backend (ADR-0008). C++ scaffold the same day:
  CMake/Ninja workspace (geom, mesh, adapt, fea, bench, cli), STL loader with
  welding, face-based mesh structure with structural validity checker,
  Material/D-matrix, backend dispatch (cpu/cuda), reference-case loader,
  CLI `check`/`backend` subcommands, Catch2 tests green.
  CI: warnings-as-errors build + ctest + clang-format + grep audit.
- 2026-07-10: P1 Tier-1 completion — Kirsch plate (SCF 3.056 vs 3), Goodier
  cavity (SCF 1.902 vs 2.045), L-domain singularity energy-gap order 1.265
  vs 2λ=1.089, Gmsh v2.2 import, GATE 1 convergence report, ADR-0009.
  37/37 tests green.

## Benchmark table
| Case | Status |
|---|---|
| Tier 0 patch test (all 4 element types, distorted meshes) | PASS, max error < 1e-12 m |
| Tier 0 rigid-body modes | PASS (< 1e-12 relative) |
| Tier 0 single-element eigenvalues (6 zero modes) | PASS |
| Tier 1 Timoshenko cantilever (hex20, gravity load) | PASS, tip err 1.50% (tol 3%) |
| Tier 1 Lamé cylinder (hex20 sector) | PASS, u_r 0.0068%, hoop 1.36% |
| Tier 1 Kirsch plate (hex20, exact field BC) | PASS, SCF 3.056 vs 3 (1.87%) |
| Tier 1 Goodier cavity (hex20 shell, b/a=15) | PASS, SCF 1.902 vs 2.045 (7.0%) |
| Tier 1 L-domain (hex20, energy-gap order) | PASS, order 1.265 vs 1.089 (±0.35) |
| Tier 2 MMS convergence | PASS: tet4 0.997, hex8 0.997, tet10 2.000, hex20 2.000 (theory 1/1/2/2, tol ±0.2) |
| Tier 2 MMS exact-representation sanity (p=2, quadratic field) | PASS (< 1e-9 relative) |
| Tier 3 performance | L-domain instrument PASS: 5.12× DOF, 12.2× time (d6-tier3); full public-suite product path still open |

## Open issues
- GATE 1 frozen; see `bench/reports/p1-gate1-convergence.md`.
- License closed: BSD-3-Clause (ADR-0002); no CLA process.
- `POLYMESH_WITH_OCC` wired in `src/geom` (`load_step`, CMake find + stub
  when OFF); exact B-rep feature queries still deferred to P3 (ADR-0001).
- CUDA SpMV scaffolding landed (F3); enable with `POLYMESH_WITH_CUDA=ON` +
  toolkit on PATH (`nvcc`). CI stays CPU-only. Batched Ke kernels still open.
  RTX 3080 Ti present (ADR-0008).
- Geometric validity: boundary manifold + tet volume checks; limited surface
  snap with Jacobian unsnap (B3) on tet and hex+pyramid fills. True Delaunay
  deferred (B1 = ADR-0015 documented limits). CAD feature queries still open.
- Goodier: exact continuum-field BCs + ZZ recovery would tighten SCF further
  (ADR-0009); P1 bar is 12% with Saint-Venant Dirichlet + nodal averaging.
