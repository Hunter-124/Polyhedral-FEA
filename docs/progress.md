# PROGRESS

## Active (read this first)

**Current program (2026-08): the learned mesh advisor corpus/retrain program.**
**Canonical docs:**
[`docs/advisor/0001-architecture.md`](advisor/0001-architecture.md) ·
[`docs/advisor/0003-training-log.md`](advisor/0003-training-log.md) (live
tracker) ·
[`docs/advisor/0010-v6-exterior-conformity-retrain.md`](advisor/0010-v6-exterior-conformity-retrain.md)
(current generation)  
**ADRs:** [0026](decisions/0026-anisotropic-metric-adaptivity.md) ·
[0027](decisions/0027-learned-mesh-advisor.md) ·
[0028](decisions/0028-boundary-conformance-hardening.md) ·
[0029](decisions/0029-independent-truth-and-honest-gates.md) ·
[0030](decisions/0030-the-ruler-was-wrong.md) ·
[0031](decisions/0031-a-jut-has-a-side.md) ·
[0032](decisions/0032-stl-order-determinism.md) ·
[0033](decisions/0033-a-gate-must-measure-what-ships.md) ·
[0035](decisions/0035-boundary-conformity.md)  
**Training box:** [`docs/training/HANDOFF-3080ti.md`](training/HANDOFF-3080ti.md)
· [`docs/training/ACCESS-hunter-pc.md`](training/ACCESS-hunter-pc.md)  
**Roadmap:** [`docs/ROADMAP.md`](ROADMAP.md) · **Agent loop:**
[`docs/process/agent-loop.md`](process/agent-loop.md)

**The 2026-07-13 measure-first / CVT board is finished and frozen.**
[`docs/dag/PROGRAM.yaml`](dag/PROGRAM.yaml) is a historical record, not the
live tracker. Its final state:

| Status | Nodes | Notes |
|--------|-------|--------|
| **Done** | **M0–M14 (all), G0–G4, V6e, V10c** | Measure path + full Geogram CVT stack (vendor→Lloyd→sites→export) |
| **Frozen, not promoted** | **M5** | Both parts **health+load_area OK**; SCF **0.545** vs hybrid 0.512, SE **0.135** vs 0.132 — [`bench/campaigns/vem-gate-m5/GATE.md`](../bench/campaigns/vem-gate-m5/GATE.md) |
| **Left open at freeze** | V6d, V11, feedback-loop | campaign-1 **done** (settings-frontier-1 finished; defaults not flipped) |
| **Deferred** | icecream face-tags | Face-ID BC design |

Board rules at freeze (ADR-0024 Q2): **freeze (done) → wall project (done) →
CVT**. Packing “win” loops measured **delta vs M9 freeze only**. Never score raw
nodal max stress.

**Post-M10 smoke (2026-07-13):** cylinder+plate_hole × varyhedron+hybrid_zoo
h_scale=5 → **4/4 `ok`**, `health_ok` + `load_area_ok` true; cylinder SE ~0.0034
vs truth 0.00393. GUI `polymesh-gui` builds with health/scorecard/load_area Results panel.

**CAD-only + geometry/BC-aware meshing (2026-07-14):** `POLYMESH_WITH_OCC` now
**ON by default**; inputs are **CAD-only** (`.step .stp .brep .brp`) — `Model::load`,
CLI, GUI, and testlab cases reject STL (parser kept only as internal test
scaffolding). New `pipeline::build_refinement_plan(model, h, regions, use_geometry)`
fuses geometry (curvature/thin-wall) + BC/load selection-box sources into one
gradient-limited seed field (`adapt::seed_plan`) driving the ball-grading meshers
(graded/hybrid/**varyhedron**). Wired into CLI `mesh`/`solve` (`--fix-box`/`--load-box`,
geometry grading default-on), the pipeline `SolveJob` bc-grading (now also fuses
geometry), and testlab (opt-in `"bc_grading": true`, baselines unchanged). Verified
end-to-end on `CD-601_v2_Seat.step`: import → geometry+BC varyhedron mesh
(224 geom + 117 BC seeds) → solve → VTU. `test_refinement_plan.cpp` added; full
suite 257/257 green (OCC on). STL box/smoke tests migrated to in-memory
`testsupport::box_model` / `model_from_surface(load_stl(...))`; cantilever+smoke_bar
cases regenerated as STEP.

**Curved-import fidelity + diagnostics + self-improve loop (2026-07-14):** Fixed
coarse pipe faceting — CAD tessellation angular deflection 0.5→0.2 rad (+ finer
linear sag), `CadModel::tessellate(deflection, angular_deflection)`; a cylinder
wall now tessellates < 1% chord deviation (`test_geometry_fidelity`). Fixed the
GUI crash `local_refine_tets: non-positive child volume`: LEB now skips a
terminal edge whose best midpoint would invert a sliver child instead of
aborting the mesh (sliver-safe, still conforming; `n_skipped_slivers`). New
`polymesh diag <part> --json` emits fidelity/quality/timing/throughput; new
`scripts/self_improve.sh [--backend omp|grok]` runs a brief CAD battery →
report → LLM CLI edits the meshers (GUI "self-improve" buttons launch it). Large
curved fixture `tests/fixtures/parts/pipe.step` (Ø60×400) added; LEB robustness
test on it. Full suite 259/259 green (OCC on).

**Meshing performance (spatial indices) + smooth CAD shading (2026-07-14):**
Profiled the frame mesh hang — all cost was brute-force closest-point over the
~112k-triangle surface, per node. Added uniform grids: `estimate_local_thickness`
O(V·F) inward ray casting → grid + DDA + OpenMP (geom now links OpenMP);
`geom::closest_on_features` → sharp-edge grid (was brute over ~16k edges/node,
26%→9% of runtime); the two boundary-region loops in `scene.cpp` now use the
grid-cached `closest_on_surface` (`.triangle`→region) instead of scanning every
triangle; `SurfaceGrid` bumped to ~2 tri/bin (res cap 64→128). Geometry refine
seeds are decimated finest-per-half-h cell (56043→229 on the frame) so the
gradient limiter / ball meshers don't choke. Net: frame hybrid mesh (28,656
elems) **25.5 s → 5.1 s (~5×)**, results unchanged, suite 259/259 green. GUI
viewport now uses crease-aware smooth vertex normals (45° threshold) so imported
curved walls (tubes/fillets) shade smoothly instead of faceted, sharp edges stay
crisp (`viewport.cpp set_model`).

**Audit-driven hardening wave (2026-08-08):** A 50-run mesher × part audit
(every product mesher against the public part suite, meshing + solving + diag,
scratch artifacts under `/tmp/mesh_audit` with per-run rows collated into MASTER
CSVs — not committed) ranked **13 defects** by severity, most of them
correctness or honesty failures rather than performance:

| # | Defect | Status |
|---|--------|--------|
| 1 | `kAuto` abandons direct LDLT at 8000 free DOF → 149–177× slowdowns, 240 s timeouts, GUI `solve_fail` | fixed + verified |
| 2 | Quality metrics hard-coded perfect for non-tet → falsified diag/campaign output, 1e22 Pa stresses trusted | fixed + verified |
| 3 | `hexvem` ~3.6× over-stiff, σ_max identically 0 | fixed + verified |
| 4 | Inverted pyramid5 cells ship to solver/VTU via a sign-blind predicate | fixed + verified |
| 5 | Inverted hex8/prism6 reach the solver → hard `solve_fail` on 3 configs | fixed + verified |
| 6 | Boundary-snap guards `vol_eps = 1e-14·h³` with no shape floor → quality collapse to 1e-17 on curved parts | fixed (slivers now caught) |
| 7 | Default hybrid mesher 12.34× over element budget on plate_hole | fixed + verified (20352 elems) |
| 8 | `global_eta` unnormalised Pa RSS → `--eta-target` dead | fixed + verified (dimensionless) |
| 9 | VEM cells exported as `VTK_CONVEX_POINT_SET`, face stream discarded | fixed + verified (`VTK_POLYHEDRON`) |
| 10 | Every prism6 inside-out per `VTK_WEDGE` winding | fixed + verified |
| 11 | CLI loads hardcoded 1000 N +Y; consistent-traction integrator dead | fixed + verified (`--load-dir`/`--force`/`--traction` + conservation check) |
| 12 | Default CLI BCs degenerate on curved parts (3 fixed nodes on icecream_cone) | **partially fixed** — sane-selection minimum + face-normal fallback |
| 13 | `varyhedron` 20–100× slower via brute-force OCC extrema | fixed + verified (~5–6×) |

Measured outcomes, all on this machine on 2026-08-08:

- **Solve auto-policy was mis-tuned by ~6×.** `SolveOptions::cg_threshold` was
  8000 free DOFs with a `kPolyVem`-only escape hatch at 100000, so ordinary hex
  and tet product meshes were pushed onto CG far below the size where the
  direct factor stops fitting. It is now **50000 for every cell type** — one
  uniform rule, no element-type special case. plate_hole hex, 11040 free DOFs:
  diag+solve **179.25 s → 1.4 s**. cantilever tet, 8232 free DOFs:
  **37.9 s → <1 s**. Above 50000 free DOFs CG still runs, now with an
  incomplete-Cholesky preconditioner (Jacobi fallback) and enforced
  iteration/tolerance bounds so a non-converging system fails instead of
  grinding.
- **Loads are now specifiable and conservative.** `solve` and `diag` accept
  `--load-dir x y z` (default `0 1 0`), `--force N` (total resultant, default
  1000 N) and `--traction Pa` (pressure; resultant = Pa × loaded-face area);
  the last of `--force` / `--traction` wins. Both are applied as a consistent
  traction ∫Nᵀt dS over the selected boundary faces, and each run prints a
  conservation check (Σ nodal load vs requested resultant, error ~1e-13).
  `diag` also takes `--fix-box` / `--load-box`, so a diagnostics run can
  reproduce a solve's boundary conditions exactly.
- **ZZ `global_eta` is dimensionless-relative.** It was an unnormalised Pa RSS
  that grew as √N, so `--eta-target` could never be reached and the adapt loop
  always ran to its pass budget. plate_hole hex now reports **η = 0.108**.
- **hex-VEM stiffness corrected.** The hex-as-polyhedron VEM path was ~3.6×
  over-stiff; cantilever tip deflection is now **1.969e-04 m** (hexvem) against
  **1.965e-04 m** (hex FE).
- **`kPolyVem` stress recovery implemented.** Poly-VEM cells previously exported
  σ ≡ 0; recovered σ_max is now **9.14e6 Pa**, matching the hex FE result. VEM
  cells also export as **VTK_POLYHEDRON (42) with a face stream** instead of
  VTK_CONVEX_POINT_SET, so polyhedral surfaces render correctly in ParaView.
- **Mesh-validity predicates fixed.** The pyramid inversion predicate is now
  sign-aware and prism/hex cells are gated per Gauss point; the sphere hybrid
  mesh ships **0 inverted cells (was 1712)**. prism6 VTK winding was corrected
  (previously 100% inside-out).
- **Quality metrics are measured, not fabricated.** Non-tet cells used to report
  a hardcoded 1.0/0.0; quality is now computed for every cell type — sphere hex
  reports **quality_min 0.028 / mean 0.663**.
- **Hybrid element budget back in range, varyhedron 5–6× faster.** plate_hole at
  `-h 0.005` hybrid now emits **20352 elements (was 118448**, 12.34× over the
  predicted budget; now ≈2.1×). `geom::project_point_on_surface` gained
  face-proximity indexing: plate_hole varyhedron **19–24 s → 3.9 s**.

Still open from the list: **#12** only has a sane-selection minimum plus a
face-normal fallback, so degenerate default BCs on strongly curved parts are
mitigated, not solved — a real face-tag BC path is still the fix.

Gating is unchanged: poly-VEM is still **not** the product default (M5 remains
not promoted) — these fixes make the VEM path correct where it is used, they do
not promote it. Tet FE remains the default accuracy claim.

**Exact-BRep boundary conformity (2026-08-17, ADR-0035):** the defect was
reported from the rendered figures — *round surfaces have weird random
defects, and edges are the same; a curved edge looks chamfered instead of a
straight 90°* — and the figures were right. Suite **434/434 green** (OCC on).

- Every Cartesian fill snapped to the **tessellation**, and only four of the
  six meshers were even given the exact oracle: plain tet and hex fill had
  zero crease awareness. `geom::project_point_on_edge` was never called by any
  mesher, so a 90° crease was reconstructed from the nearest point of a
  *face*; varyhedron's 35 % "edge attraction" blend then chamfered the
  remaining 65 % by construction. The unsnap ladder quietly kept partial
  projections: 9 of 584 moved nodes on icecream_cone fully retreated, yet the
  worst node sat 0.60 h off the CAD.
- New `mesh::pin_feature_nodes` hard-pins boundary nodes onto exact CAD
  vertices and sharp edge curves, all-or-nothing, with separate capture
  (0.5 h) and travel (0.35 h) radii. Closed crease chains are re-spaced
  through a periodic low-pass of their arclength-parameter deviation
  (`geom::lowpass_signal_periodic`, moved out of `adapt::spectral` so a mesh
  pass can reach it): **Fourier chooses where along the curve a node sits,
  never where the curve is.**
- `snap_boundary_nodes` gained a `RelaxNeighborhoodFn`, called whenever the
  projection cannot be kept whole; tet/hex fill relax star interior nodes and
  fall back to tangential re-projected wall nodes when the stair cell has no
  interior corner. The ship gate `relax_cells_below_shape_floor` re-measures
  every emitted cell with `fea::cell_quality` and reports the remainder as
  `mesh.n_below_shape_floor` (ADR-0033's rule at the last hand-off).
- Measured at h = 8 mm, boundary-node p99 over h: sphere graded 0.039 →
  **2.9e-15**, cylinder graded 0.045 → **2.8e-15**, plate_hole tet 0.218 →
  **0**, icecream_cone graded 0.052 → **3.4e-15**. Normal-angle p99 30° →
  **4.4°**. Inverted cells gone: sphere hybrid h = 3 mm quality_min −0.837 →
  +0.0022 with 38 cells honestly reported below the floor; icecream_cone
  hybrid −0.742/−0.779 → +0.020 with none.
- Still open and documented rather than hidden: see the second wave below, which
  closed most of it and measured the rest properly.

**Exterior conformity + facet kinks (2026-08-17, ADR-0035 §5–6):** the defect
was reported again from the figures — *still seeing visible surface defects on
the sphere and on the icecream cone* — and again the figures were right. Suite
**435/435 green** (OCC on).

- The first wave conformed each mesher's own lattice skin; what ships is
  `fea::extract_boundary_faces`, the true element exterior. A fan tet peeled
  after the snap, a pyramid emitted as two tets, an LEB child carved late — all
  expose nodes that were interior when the snap ran. sphere/hybrid's own worst
  boundary node read 0.016 h while the shipped mesh carried nodes 0.085 h off
  the BRep. New mesher-independent `conform_true_exterior` gate.
- Three mechanisms it needed: **exact resolution** (the owner-oracle fallback to
  the tessellation reported ~0 for exactly the wrong nodes, because OCC's own
  facets are 0.085 h off this sphere), a **constrained march** with interior and
  tangential-wall room opening, monotone in the *measured* free distance, and
  **conforming hex relief** — a hex saturated at the shape floor is fanned into
  six pyramids over its own faces, gated per child and rolled back if it buys
  nothing.
- New `fea::element_jacobians_positive`: `cell_quality` is a shape measure, and
  a quality-accepted move shipped det J = −6.085e-09. Every repair pass now
  gates on the assembly's own rule, and the ship gate counts non-integrable
  cells. Additive — no GATE-1 file changed.
- **Facet kinks** were the visible defect: the showcase cone shipped adjacent
  facet planes differing by up to 77.8°, because grading transitions leave
  needle facets beside bulk ones. Kink relief slides face-owned nodes along
  their own surface to lower the worst local kink; crease and corner nodes are
  never slid. The dihedral detector also compared *signed* normals, so an
  opposite winding read as a 180° crease — 177 reported segments where the
  geometry has 47, which was most of the previously-recorded "spurious creases".
- Measured at h = 8 mm: cone/varyhedron feature p99 **17.3 → 0.150 h**,
  cone/hybrid **1.16 → 0.025 h**, sphere/hybrid node p99 **0.0062 → 5.9e-15**,
  sphere/hexpyr node p99 **0.373 → 0.026**, sphere/octa **0.150 → 0.021**,
  cylinder/hybrid normal p99 **19.2° → 0.27°**, cone/graded normal p99 **20.5°
  → 9.12°**. No case regressed; the whole pass reverts wholesale if worst
  quality drops, sub-floor count grows, or any cell becomes non-integrable.
- The uniform Cartesian `tet` fill remains floor-bounded and is no longer the
  product default. Three measured refinement schemes (more snapping, centroid
  Steiner relief, conformity-driven LEB) all violated the shared shape floor.

**Authoritative curved solve geometry (2026-08-17):**

- The display-only approach was removed. CAD-backed product mesh/solve paths
  now use the same projected quadratic volume mesh for stiffness assembly, VTU
  export, diagnostics and Studio. `SimSetup` and CLI product defaults select the
  graded path with curved solve geometry enabled.
- Curvature-dominant BReps use a 0.5× accuracy lattice. Pyramids are converted
  through the assembly-consistent diagonal; tet4/hex8 cells are promoted to
  tet10/hex20; exact face and sharp-edge projections are accepted only above
  `quality = 0.02` with positive sampled Jacobians.
- A boundary-graph sharp-edge pass now pins connected CAD-edge segments as
  coupled endpoint moves. When that pin would consume the curved-cell reserve,
  a deterministic local pattern search moves only the affected interior star;
  the move is committed only when every touched cell clears the ship gates.
- Studio no longer draws four flat triangles as a proxy for each quadratic
  face. It evaluates the solved isoparametric surface at eight subdivisions and
  applies the same interpolation weights to displacement, von Mises,
  displacement magnitude and nodal η.
- Graded h = 8 mm rounded corpus, actual solved mesh:
  sphere p99/bbox **7.12e-6**, qmin **0.02542**, normal p99 **0.335°**;
  cone **5.50e-5**, **0.02460**, **2.59°**; cylinder **5.66e-6**,
  **0.03250**, **0.195°**; plate-hole **4.83e-5**, **0.02008**,
  **0.818°**. All four are below the **1e-4 / 99.99%** surface target,
  with zero inverted and zero sub-floor cells.
- Exact sharp-edge diagnostics now classify CAD-owned quadratic boundary edges
  and use exact OCC curve projection in the mesh→BRep direction instead of the
  sampled-polyline distance floor. Cone CAD-edge coverage p99 is 23.6 μm at
  h = 8 mm; all BRep vertices are exact.
- Advisor labels changed because the product default, element order, DOF census
  and rounded-part mesh counts changed; generation-v7 archive/regeneration and
  retraining are required rather than reusing v6 labels.

**Spectral sizing + coarsening + budget advisor + CG equilibration (2026-08-16,
ADR-0034):** one wave, four mechanisms, suite **432/432 green** (OCC on).

- New `adapt::spectral` module: deterministic double-only radix-2 FFT; CAD-edge
  κ(s) denoise (even-reflect + 99.5% energy truncation) feeding chordal size
  sources at the constant-relative-sag rule; Cartesian-grid spectral trimming
  of the fused size field with a geometry-only floor re-imposed afterwards;
  `enforce_element_budget` (truncate → one uniform h scale) for
  density-contract callers. CLI mesh/solve/diag default ON (`--no-spectral`
  opts out; library `SimSetup.spectral_smooth` and testlab `"spectral_smooth"`
  default OFF — frozen baselines untouched). Measured: sphere h=0.008 seeds
  51→41 with an identical 9,194-elem mesh; icecream_cone 95→37 seeds plus 43
  denoised edge seeds, quality/stress/BRep-p99 bit-identical to base.
- Coarsening lands: `dorfler_coarsen_mark` (insignificant tail, θ=0.02) +
  `HpAction::kCoarsen` (lowest priority; only overrides kNone) gated on exact
  per-element sizes (cube-root volume — the global-h proxy is gone from the
  signal builder) vs the a-priori fused-field demand (≥1.5× finer). The adapt
  loop suppresses the LEB fallback on coarsen passes, allows a bounded global
  h rise (×1.25/pass, capped at the resolved h), and will not early-stop with
  coarsen marks pending. plate_hole / cantilever adapt-2/3 campaigns green;
  firing domain documented in the ADR (p-raise legitimately claims smooth
  elements first on those parts).
- Advisor: `recommend(..., max_dof)` budget-feasible gated enumeration +
  `budget_refusal` honesty path + CLI `--advisor-max-dof`. Measured on
  box_hole_s0: cap 3000 flips the choice from graded_tet p2 (pred DOF 66k) to
  hex p1 (pred DOF 2.8k). Stale 43-column comments corrected to the true
  62-column contract (geo_* are network inputs AND the OOD space); testlab
  `curved_frac` no longer saturates (serving-side κ fraction; dataset was
  verified already honest — no retrain implied).
- CG: symmetric diagonal equilibration S·K_ff·S before the IC→Jacobi cascade,
  with acceptance still measured on the original-space true residual.
  Direct path and GATE-1 frozen files untouched; measured 58 iterations to
  1.6e-9 on a diagonal-spread-10⁶ MPC case (LDLT parity < 1e-5); live on the
  sphere solve smoke.
- Deliberately not wired (measured): the spectral budget scale is not coupled
  to lattice-mesher element ceilings — grid Σvol/h³ vs real counts diverge
  ~20× part-dependently (sphere cap-8000 run: 432 predicted vs 9,456 actual),
  so resolve + auto-retry remain the cap authority.

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
- 2026-08-15: **Mesher-quality wave — a gate measures the cell that ships
  (ADR-0033)** — pyramid/hex/graded gates now measure the cell that ships:
  corner-folded pyramids ship as their two assembly split tets
  (`icecream_cone` h=0.008 `quality_min` **−0.742 → +0.0201** at unchanged
  p99/h); hex interior relaxation between snap rounds (sphere M1max
  **6.9e-4 → 1.1e-16**); `snap_boundary_nodes` acts on its final sweep to a
  fixed point (`ellipsoid_boss_s1` hybrid −0.9939 → +0.0200, 4 inverted cells →
  0); graded interior-sliver relaxation (`cylinder` h=0.005 worst aspect
  **4.17e-05 → 5.86e-04** at identical element count). Three new non-circular
  curved corpus families: `lobed_shaft`, `twisted_loft`, `ellipsoid_boss`;
  `lobed_shaft` hybrid converges (exact-BRep p99/h 0.0191 → 0.0105, boundary
  normal p99 26.8° → 0.2°). `diag` reports `quality_min_type` +
  `n_inverted_cells`. **Two open defects (ADR-0033):** the graded sliver chain —
  `cylinder` graded h=0.005 still builds a mesh CG cannot solve (min edge
  0.004 h; needs a graded-snap re-engineering, not a threshold change) — and the
  `ellipsoid_boss` boundary tail (binding constraint `hex8_shape_quality >=
  0.02` vs required wall travel; next thread is the size field, not the snap).
  Advisor labels carrying element counts for folded hybrid meshes are stale —
  corpus regeneration required before the next retrain. Suite 409/409; library
  embeddable, builds on Eigen 3.4 (`aa5ae76`).
- 2026-08-14: **v4 corpus regeneration + retrain
  ([`docs/advisor/0008-v4-corpus-retrain.md`](advisor/0008-v4-corpus-retrain.md))**
  — all MSVC-era v3 rows archived under `bench/campaigns/archive-v4/`; 3,528
  pairs re-measured across hunter-pc (gcc 15) + livingroom-pc (gcc 16); the 488
  pairs solved on both hosts agree **exactly** — ADR-0032's determinism holds at
  corpus scale. Truth campaign dropped (every reference is now `analytic` or
  `external-gmsh-mesh+calculix-solver`, promotion-proof: `promoted=0,
  protected_refused=188`). The retrain found the fold eating the headline:
  `smoke_bar`'s reference scored raw nodal max von Mises on a fully clamped bar
  — a quantity that diverges under refinement — so the advisor was charged ~3
  decades for correctly picking a finer mesh. `load_metrics` now rejects
  `max_von_mises` references; fold regret **2.961 → 1.114**, best validation
  `rel_err_mae` **0.6893 → 0.5201**, advisor leads `random` on the raw macro
  mean (0.601 vs 0.687). Shipped v4: 2,752-row dataset, ONNX 62 columns parity
  2.2e-06, `plate_hole --advisor` → graded_tet exit 0 (HANDOFF-3080ti §2a
  acceptance holds). Tolerance selector re-measured on corrected labels: gap vs
  `finest_action` closed 3× → 1.6×, still misses the promised tolerance — not
  shipped.
- 2026-08-14: **STL-order determinism (ADR-0032)** — mesher output depended on
  `unordered_map`/`unordered_set` iteration order reaching mutations (hybrid
  smoother, snap order, surface/wall project, varyhedron, hp DOF numbering):
  replaying 24 (part, cfg) pairs MSVC→gcc reproduced only 19. Fix: iterate
  sorted ids wherever order drives a mutation, selection, or index assignment.
  `scripts/check_cross_stdlib_mesh.sh` (libstdc++ vs libc++, byte-identical
  meshes on 4 part/mesher pairs) runs in CI; 409/409 ctest on the fixed tree.
  Labelling standardises on gcc.
- 2026-08-14: **Clean-data retrain
  ([`docs/advisor/0006-clean-data-retrain.md`](advisor/0006-clean-data-retrain.md))**
  — campaign regenerated on the post-untangle mesher: 3,672 rows re-measured;
  corpus successes 54.7 % → 70.0 %, fill-stage guard refusals 28.4 % → 19.2 %.
  Scale-law features added (`log10_volume/diag/h/cells`, mirrored in
  `advisor.cpp`). `best.pt` now tracks best validation (`latest.pt` was 13 %
  worse). Macro-mean regret ranked the advisor below `random` — answered in
  0008 §4: a bad label, not a bad model. Refusal rows measured as family-local
  noise (LightGBM AUC 0.372 on box_hole — worse than chance); the exclusion
  stands as a measured result.
- 2026-08-14: **"Cheapest mesh within X" measured, not shipped
  ([`docs/advisor/0007-tolerance-selector.md`](advisor/0007-tolerance-selector.md))**
  — tolerance selector scored over 12 family-held-out folds: selectors are 2–5×
  cheaper than `finest_action` among satisfying picks but violate the tolerance
  1.3–2.6× more often; no safety margin on a 0–2 decade grid reaches 10 %
  violation. Ships as scorer + published number, not a feature.
- 2026-08-13: **ADR-0030 "the ruler was wrong"** — the "fan transition loses
  36–40 % of volume" defect **retracted**: `fea::pyramid_rule` integrated the
  collapsed-brick map over the wrong domain (the exact rational 0.6); the rule
  is now tensor-Gauss on the cube with a per-element volume-exactness test.
  Display path splits quadratic faces through their mid-edge nodes (60° hex20
  cylinder sector worst gap **4.019 → 1.022 mm**). One `fea::element_volume`
  definition. Graded torn shells fixed — geometry deletion now proposes and the
  shell disposes: **14/14 watertight** across both meshers and 7 parts;
  `tube_s0` h=0.00125 graded rel_err 0.2446 → 0.09097; boundary-shell census in
  every mesher note; `score_volume` requires closure before measuring.
- 2026-08-13: **ADR-0031 "a jut has a side"** — the graded S5 void carve peeled
  juts that were far from the surface but *inside* the solid (sphere craters);
  a jut is now far **and** outside (one shared `outside_solid` oracle). Every
  hole-bearing part re-measured bit-identical; graded-sphere test pins 0 carved
  nodes.
- 2026-08-13: **ADR-0029 independent truth + honest gates** — 64 of 72 corpus
  references were self-generated (`overkill-reference`): truth now comes from an
  external **Gmsh 4.13.1 + CalculiX 2.23** chain touching neither our mesher nor
  solver (88 external + 8 closed-form references, validated against closed form
  before adoption; our solver cross-checked to 3.4e-09 tip deflection on an
  identical Gmsh mesh). Applied load made mesh-independent (traction rescaled by
  `cad_rule_area / mesh_selected_area`; measured deficits up to **28.2 %**);
  load-area gate is three-valued (`verified` / `rescaled_to_exact_cad` /
  `unverified` — unverifiable is never a pass); truth promotion is an allowlist
  (`scripts/truth_guard.py`); accuracy re-derived from raw `answers` at dataset
  build time. Reference values moved a median **3.79 %**, up to **+88.4 %**.
- 2026-08-13: **Advisor honest evaluation + gated enumeration ships** — the
  part-hash split leaked (672/672 validation rows had a training twin);
  splits are now leave-one-family-out with deployable baselines only. The
  shipped chooser is feasibility-gated enumeration: `advisor_gated_0.05` 0.3468
  `rel_err` regret at 10.0 % pick-failure vs `advisor_argmin` 0.4413 at 22.7 %,
  paired sign test **38W-0L-262T, p = 7.3e-12**; ranking alone does not beat
  `finest_action` (117W-136L-47T, p = 0.258) — the gate earns the win.
  Calibration + OOD veto wired in (was validated but inert); cards and figures
  regenerated with provenance.
- 2026-08-12: **ADR-0028 boundary-conformance hardening** — order-2 mid-edge
  nodes projected onto the exact B-rep behind a stiffness-quadrature validity
  gate (two scaled epsilons, six bisection backoffs); `peak_vm` probes for peak
  truths (the box_hole mean probe had a ~0.66 error floor vs the Kirsch peak);
  `vtu_wire_png.py` renders by VTK topology; LEB termination guaranteed
  (projected midpoint accepted only if both children ≤ 0.75× parent); ZZ patch
  recovery bounded (SVD + L2-gain guard: box_hole_s2 probe max 10.79 → 2.979 MPa
  vs Kirsch 3.0; external row rel err 2.595 → 0.0072). Row schema bumped
  `advisor-row-v2` → `-v3`; retrain on post-fix rows only.
- 2026-08-10: **Learned mesh advisor trained + wired into solve (ADR-0027, v2)**
  — first trained model (training log M-A1): 24 procedural parts × 3 loads = 72
  cases, batch 1 1536/1536 pairs, truth coverage 67/72; inference ships through
  ONNX Runtime CPU. Advisor picks meshes ~6× more accurate than the default
  where truth is real (`7cfaabf`). Also: solve imported Gmsh `.msh` meshes; tet10
  mid-node import order fixed (`92455f9`).
- 2026-08-09: **ADR-0026 variable-everything wave 1** — anisotropic metric
  contract shipped as `adapt::Metric3d` + `adapt::MetricGrid`; real scalar size
  fields now reach the graded/hybrid meshers via `mesh::SizeFieldFn`
  (field-derived levels replace the binary ball test, 2:1 conformity preserved);
  exact packed RVD cells admitted (`46cbdc1`); OpenCASCADE enabled on Windows.
- 2026-07-14: **CVT poly mesher perf + poly VEM stress** — neighbour-restricted
  clipping O(N²)→O(N·k); export emits one polyhedron per site (was fragmented
  per tet); GUI renders polyhedral cells as true polygon facets; poly VEM
  recovers von Mises stress (was identically 0) + direct solver for VEM
  (`2789062`, `d0ec1ad`, `0e0532e`, `e57f73b`).
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
  *(Superseded 2026-08-08: threshold is 50000 for every cell type, with an
  incomplete-Cholesky preconditioner and bounded iterations — see "Audit-driven
  hardening wave".)*
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
