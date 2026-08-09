<!-- Generated 2026-08-09 by a research subagent for the variable-everything + advisor program. Raw, unedited. -->

# Training-label generation at scale

# Training labels at scale for a PolyMesh mesh-parameter advisor

## Executive finding

The published systems do **not** learn the full PolyMesh action space. They mostly learn one of four narrower objects: a scalar sizing field, an elementwise error indicator, an anisotropic metric, or a sequential refine/no-refine policy. The closest scalable recipe for PolyMesh is therefore **not** to assign one brittle “best configuration” class per problem. Record the complete outcome vector for every sampled `(CAD, BC, mesh action)`—errors, DOFs, time, memory, and failure—then learn an action-conditioned outcome/ranking model. Derive Pareto-optimal actions at query time. Generate a separate scalar `log h(x)` label from a converged adaptive expert mesh, because PolyMesh cannot consume metric tensors.

## 1. What published systems use as the label

| Work | Exact learned target | How the target is obtained | Consequences |
|---|---|---|---|
| **MeshingNet** (Zhang et al., 2020) | Regression of a **local target triangle-area upper bound** `A(x_i)` at every coarse-element centroid. They set `A_i = K/E_i^alpha`, with `alpha=1`; `K` controls total element count. | Solve the same problem on a low-density uniform mesh and a much finer high-density uniform mesh; interpolate the coarse solution to the fine mesh, compute local solution error, and project it back. | Directly drives Triangle and is simple supervised regression. But it bakes in an error norm, budget, element family, and expert conversion rule. Source: [paper](https://arxiv.org/abs/2004.07016). |
| **LAMG** (Zhang et al., 2025 preprint) | Regression of a **scalar target element edge length** at sampled volume points. The label is the regular-tetrahedron edge length corresponding to the containing expert tetrahedron’s volume. | Run ZZ-driven AMR from roughly 5,500 to roughly 15,000 vertices; query the resulting adaptive mesh. | Topology-independent and immediately usable by Gmsh/FTetWild. It imitates the chosen AMR policy rather than a unique mathematical optimum. Source: [paper](https://arxiv.org/abs/2505.20457). |
| **AMBER** (Freymuth et al., 2025 preprint) | Vertexwise regression of an expert mesh’s **scalar average edge length**, projected from the expert mesh to each model-generated intermediate mesh. | Expert meshes come from error-indicator refinement, task heuristics, or humans. Model-generated meshes are automatically relabeled by point lookup in the expert mesh, giving DAgger-like replay without a human in the loop. | Particularly relevant to PolyMesh: expert labels can be recycled onto arbitrarily many intermediate meshes. It still learns isotropic size only and inherits expert bias. Source: [paper](https://arxiv.org/abs/2505.23663), [project](https://niklasfreymuth.github.io/AMBER/). |
| **E2N** (Wallwork et al., 2022) | One real number per element: the **signed element contribution to a dual-weighted-residual goal-error estimator**. It does not directly predict a mesh. | A costly globally enriched adjoint computation supplies the target; inputs remain on the base mesh. A separate metric construction uses the predicted indicator plus recovered forward-solution Hessians. | Error prediction is reusable across target budgets and adaptation algorithms and keeps sign/QoI information. It requires a base forward and adjoint solve and still needs an indicator-to-metric policy. Source: [paper](https://arxiv.org/abs/2207.11233). |
| **RL-AMR** (Yang et al., 2023) | No supervised label. The action is **choose one element to refine** (plus no-op); the dense reward is normalized reduction in global L2 error. Without truth, the proposed surrogate reward is the norm of the difference between solutions with and without the chosen refinement. | Analytic parametric functions, high-resolution reference simulations, or the one-step surrogate reward. | Optimizes a sequential objective and can outperform greedy marking, but label generation becomes environment interaction: 10,000–20,000 episodes for their main tasks. Source: [PMLR paper](https://proceedings.mlr.press/v206/yang23e.html). |
| **ASMR** (Freymuth et al., NeurIPS 2023) | Each element-agent makes a **binary mark/don’t-mark action**. Its local reward is reference-solution error reduction, divided by element area, minus a penalty for added elements; local and global returns are averaged. | Compare the current solution at fine-reference element midpoints against a uniformly six-times-refined reference mesh. | Directly learns cost/accuracy tradeoff and credits each element, but reward design and reference meshes are expensive. Source: [paper](https://proceedings.neurips.cc/paper_files/paper/2023/hash/e85454a113e8b41e017c81875ae68d47-Abstract-Conference.html). |
| **AdaptNet** (Després et al., 2024) | A GNN predicts components of a **Hessian-based anisotropic metric** (or predicts the field from which it is constructed); another network predicts initial mesher parameters. | Simulation-derived solution/Hessian information. | A metric encodes size, stretching, and orientation and is more DOF-efficient for layers/wakes. It must remain SPD and transform correctly under rotation; PolyMesh currently cannot consume it. Source: [journal article](https://doi.org/10.3390/math12182933). |

### Comparison of target choices

- **Scalar `h(x)` / local volume:** best immediate target for PolyMesh. Regress `log(h/L)` rather than raw metres to enforce scale behavior and handle dynamic range. It is easy to query at refinement seeds, but cannot encode anisotropy and is non-unique: many meshes achieve the same error/cost.
- **Error/error-density:** more reusable than a mesh label. It permits changing budgets and marking rules after training. Its drawbacks are extreme dynamic range, possible signed cancellation for goal-oriented indicators, and the need for a current solve plus a downstream adaptation rule.
- **SPD metric field:** richest local target and the established representation for anisotropic adaptation, but unusable until PolyMesh has a metric-aware mesher. Training it now would create labels with no executable action.
- **Binary refine/don’t-refine:** easy output, but a thresholded expert indicator throws away magnitude and makes labels depend on budget, current mesh, conformity closure, and threshold. Positive labels are often sparse.
- **RL reward/action:** appropriate when long-term refinement order matters. It is much more expensive and unstable than supervised outcome learning, and is unnecessary for choosing a one-shot global `SimSetup`.
- **One global “winner” class:** avoid this for PolyMesh. Near-tied configurations will flip labels under timing noise or a small utility-weight change, and an unevaluated action can never win. Preserve the measured outcome vector and Pareto set instead.

**Novelty boundary:** others have learned local size, indicators, metrics, and refinement policies. I found no published system that jointly chooses CAD/BC-conditioned `mesher`, global `h`, shape tendency/mix, skin layers, grading, `p`, adaptation controls, and a local scalar sizing field against measured accuracy/DOF/time. That combined action-conditioned advisor would be novel.

## 2. Ground truth when no analytic solution exists

### What the papers actually do

1. **Over-resolved reference solve is the dominant operational truth.** MeshingNet treats its high-density uniform solve as truth. RL-AMR uses a highly resolved reference for Burgers. ASMR uniformly refines six times. LAMG evaluates solution error against a 200,000-vertex FEM solve. This is straightforward, physics-faithful, and expensive—especially in 3D, where halving `h` can multiply element count by about eight.
2. **An adaptive/enriched expert can label mesh need without being exact truth.** LAMG uses ZZ-driven AMR as its sizing-field expert. E2N uses a globally enriched adjoint solution to label dual-weighted residual contributions; it explicitly calls this accurate but still approximate and computationally expensive. AMBER accepts heuristic and human meshes as experts.
3. **Superconvergent recovery is usually an estimator or expert, not truth.** ZZ recovery is LAMG’s AMR driver and a baseline in ASMR. It is cheap and suitable for weak labels, but it should not be described as exact solution error.
4. **Manufactured solutions are excellent verification truth.** Schneider et al. prescribe an analytic displacement/solution, derive body force and boundary values by substitution, and run Poisson and elasticity on 3,200 real geometries. This proves manufactured solutions scale across arbitrary domains: [paper](https://arxiv.org/abs/1903.09332); classical MMS reference: [Salari & Knupp](https://www.osti.gov/biblio/759450). However, the induced loads/BCs need not resemble real fixtures and tractions, so MMS should be an audit set, not the main advisor corpus.
5. **Richardson extrapolation is useful but was not the primary label method in the learned-meshing papers above.** It estimates a scalar QoI limit cheaply from multiple resolutions only after the sequence is demonstrably in the asymptotic regime. Remeshing, changing element types, singular stresses, and adaptive/non-nested meshes can invalidate its assumptions. Use it to audit compliance/energy references, not as the sole local field truth.

### Cost implications

- A fine reference label is amortized: one reference solution can score many candidate mesh configurations for the same geometry/BC.
- Do not retain every candidate field. Retain reference fields, adaptive expert checkpoints, and compact outcome summaries for ordinary candidates.
- For arbitrary 3D elasticity, a converged adaptive quadratic-tet reference is more affordable than repeated uniform `h` halving and less correlated with the candidate’s hybrid element family.
- Stress maxima at point loads, re-entrant corners, or sharp fixture transitions may be mathematically singular. Label a region-averaged stress or volume-weighted 99th percentile, not an unconverged raw maximum.

## 3. How boundary conditions are randomized

The literature usually samples **within a hand-designed valid BC template**. It rarely selects arbitrary CAD faces uniformly.

- **MeshingNet elasticity:** 6–8-edge polygons; edge 1 is always fixed, edges 4–5 always receive uniform pressure/traction, all others are free. Traction amplitude is random up to 1,000; density and Poisson ratio are randomized. This teaches parameter awareness but not fixture/load topology generalization ([paper](https://arxiv.org/abs/2004.07016)).
- **LAMG:** each Poisson problem uses 40–50 random Gaussians for Dirichlet boundary data; Gaussian centers are projected onto the boundary to avoid trivial BCs on thin shapes. It also samples 20–30 spherical source terms with random radii. This is concrete, broad functional randomization, but not structural mechanics ([paper](https://arxiv.org/abs/2505.20457)).
- **ASMR elasticity:** on randomized L-shaped domains, one side has prescribed displacement and the opposite side is zero; displacement direction is sampled uniformly over `[0, pi]` and magnitude over `[0.2, 0.8]`. Its Poisson task samples three anisotropic Gaussian load components with random means, rotated log-uniform covariances, and weights. The paper provides exact schemes in Appendix B ([paper](https://proceedings.neurips.cc/paper_files/paper/2023/hash/e85454a113e8b41e017c81875ae68d47-Abstract-Conference.html)).
- **E2N:** it keeps the rectangular domain/inflow BC family fixed but samples 1–8 turbines, positions them subject to separation, and samples viscosity, bathymetry, and inflow speed. This is process-parameter/domain randomization, not arbitrary BC topology ([paper](https://arxiv.org/abs/2207.11233)).
- **SimJEB:** 381 real STEP brackets share known interfaces and four fixed structural load cases. It is valuable precisely because the geometry corpus and operating conditions are functionally aligned; random scraped CAD lacks that property. Dataset is ODC-By and separately zipped by STEP/FEM/VTK/OBJ/CSV: [paper](https://arxiv.org/abs/2105.03534), [dataset](https://simjeb.github.io/).

### Recommended structural BC sampler

1. Identify admissible support/load faces from CAD semantics where available; otherwise classify planar/cylindrical faces and connected face patches. Do **not** sample tiny fillet/chamfer faces uniformly.
2. Sample one support patch, occasionally two, and reject setups whose constrained stiffness still has rigid modes. Sample disjoint load patches, weighted by usable face area.
3. Balance mechanisms rather than magnitudes: normal pressure, tangential traction, remote/distributed force, bending, and torsion; sample 1–3 simultaneous loads.
4. Reject near-zero strain-energy cases, load/support overlap, and point singularities. Record every rejection reason.
5. In linear elastostatics, multiplying all loads by a scalar multiplies displacement/stress but does not change the relative discretization need. Unless training for an **absolute** error tolerance, do not waste cases on load magnitude alone; vary direction, footprint, and fixture topology.

## 4. Data volume actually used

| Work | Independent problems/geometries | Effective local samples | Relevant ablation/cost evidence |
|---|---:|---:|---|
| MeshingNet | 3,800 random problems: 3,000 train, 800 test | About 1,000 coarse elements/problem, hence over **3 million** training pairs | No strong data-count ablation; 2D constrained polygon family. |
| LAMG | Models used 1 shape/1,000 PDEs; 5/1,000; 100/1,000; 500/5,000; 2,500/25,000 | 200–2,000 sampled points per PDE | The 500-shape/5,000-PDE model was within about 1% of the 2,500/25,000 model while the larger model cost about 30% more to train. Large evaluation used 582 Thingi10k shapes. |
| ASMR | **100 training PDEs**, 100 disjoint evaluation PDEs per task | Every mesh element/step yields agent experience | Ablation at 1, 10, 100, 1,000 PDEs: fewer than 100 were less stable; performance stabilized around **100**, with only minor gain at 1,000. This is the clearest minimum-data evidence. |
| AMBER | Usually only about **20 geometries** per dataset; Mold uses 18 training geometries x 3 inlet positions = 54 condition-specific expert meshes | Expert size is projected to every vertex of replay-buffer intermediate meshes | Shows that strong expert mesh structure plus automatic relabeling can work with tiny geometry sets; each task is narrow and trained separately. |
| E2N | 100 randomly generated scenarios, each initial + two adapted meshes = **300 meshes** | **711,307 elements**, split 7:3 | Data/feature generation about 100 minutes and network training about 101 minutes in the reported 2D setup. |
| RL-AMR | New parametric function sampled each episode; 20k static, 10k advection, 2k–4k Burgers episodes | 10–50 actions/episode | 6–9 CPU-hours static and 14–18 CPU-hours advection on one Xeon core, depending on architecture. |

A million element rows from 100 PDEs are **not** a million independent geometries or BC cases. For geometry/BC generalization, count the top-level physics problems. The evidence supports roughly **100 independent PDE cases as a lower bound for a narrow local policy**, 1,000–5,000 for broad shape/BC sizing-field generalization, and surprisingly few geometries only when expert meshes and strong replay augmentation are available.

## 5. Manufactured and procedural geometry

### What has been done

- MeshingNet procedurally samples 2D polygons; ASMR samples L-shapes, square holes, and perturbed convex polygons; AMBER creates 2D beams with sequentially placed circular cutouts. These are useful PDE randomizers but are not realistic 3D CAD parts.
- **CAD-Recode** generates one million executable CadQuery programs using circles/rectangles/arcs, Boolean union/cut, randomized sketch planes, and extrusion. Every program creates a parametric B-Rep. Its scope is explicitly limited: the paper says the synthetic set lacks operations such as fillet and revolution. Source: [paper](https://arxiv.org/abs/2412.14042), [dataset card](https://huggingface.co/datasets/filapro/cad-recode). The downloadable dataset is **CC BY-NC 4.0**, so it is unsuitable for unrestricted commercial reuse; it is still a reproducible recipe. CadQuery itself is an actionable Python CAD front end: [documentation](https://cadquery.readthedocs.io/).
- **FllumaOne** (June 2026 preprint) reports 100,000 OpenCASCADE-kernel-validated STEP solids across 53 procedural template families, including plates, motor and pipe brackets, mounting blocks, lofted brackets, ribbed mounts, holes, pockets, bosses, chamfers, fillets, sweeps, shells, and patterns. It retains only valid solids and successful STEP exports: [paper](https://arxiv.org/abs/2606.17696). Important availability caveat: the paper names [this repository](https://github.com/Cad-Kernel/FllumaOne-100K), but the repository returned HTTP 404 when checked, and the paper’s CC BY 4.0 license does not by itself establish a dataset/software license. Treat it as a demonstrated generation design, not currently dependable downloadable data.
- Schneider et al. apply manufactured Poisson/elasticity solutions to 3,200 automatically meshed real geometries, proving that arbitrary-shape simulation labeling can be automated even when geometry was not procedurally generated ([paper](https://arxiv.org/abs/1903.09332)).

### Actionable generator for PolyMesh

Use PolyMesh’s existing OpenCASCADE stack—or CadQuery for rapid authoring—to define 10–20 engineering template families: plates with hole/pocket patterns, clevises, L/U brackets, ribs/gussets, flanges, hollow boxes, pipe supports, swept parts, and filleted/re-entrant variants. Randomize dimensions in dimensionless ratios, operation order, feature count, thickness, hole spacing, fillet radius, and deliberate small-feature regimes. Keep construction semantics so the BC sampler knows mounting and load faces. Run kernel solid-validity, positive-volume, minimum-thickness, and STEP round-trip checks. This is cheaper and much more simulation-usable than decorative triangle soup.

## 6. Domain randomization and synthetic-to-real transfer

Evidence exists, but not yet for the exact PolyMesh task.

- CAD-Recode is the strongest synthetic-to-real geometry result: training on procedural CadQuery programs improved reconstruction on human CAD datasets and real CC3D scans; on CC3D it reports 89% lower median Chamfer distance and 30% higher IoU than CAD-SIGNet. This demonstrates transferable CAD geometric priors, **not** transferable mesh-parameter decisions ([paper](https://arxiv.org/abs/2412.14042)).
- ASMR’s 100 randomized synthetic PDEs generalize to larger spiral domains and meshes over 50,000 elements after scale-oriented augmentation. RL-AMR transfers from static projection to advection and from 8x8 training meshes to much larger meshes. These support local/equivariant policies and domain randomization, but their domains remain synthetic 2D PDE families.
- LAMG trains across hundreds to thousands of Thingi10k shapes and randomized boundary/source functions. Its inputs are watertight boundary meshes, not feature-preserving mechanical CAD, and Thingi10k includes printable/decorative shapes.
- AMBER handles synthetic PDE geometries and real industrial meshes, but uses separate per-dataset training; it does not establish synthetic-to-real cross-domain transfer.

**Conclusion:** there is no convincing controlled result showing that a mesh advisor trained only on synthetic mechanical solids transfers to arbitrary real CAD under structural BCs. Train with a real-CAD minority and make the test split real and family-disjoint. Synthetic-only performance should be reported separately.

# Concrete, costed PolyMesh protocol

## A. Dataset composition and split

- **200 geometries total**:
  - 120 procedural solids from 12 engineering template families (10 variants/family).
  - 80 cleaned real STEP/BREP solids with meaningful load interfaces, including a functionally consistent source such as SimJEB where licensing permits.
- Split by entire template/design family, never random near-duplicate instances:
  - 150 train, 20 validation, 30 held-out test.
  - Ensure at least 20 of the 30 test geometries are real CAD.
- **6 valid BC cases per geometry** using the sampler above: **1,200 independent CAD/BC problems**. This is above ASMR’s saturation point and near LAMG’s 1,000-PDE broad-shape experiment while remaining workstation-scale.

## B. Per-problem mesh campaign

Evaluate exactly **24 mesh configurations per CAD/BC problem**:

1. 10 coverage anchors—one feasible anchor for each PolyMesh mesher.
2. 8 maximin/Latin-hypercube actions over normalized `mesh_size`, `element_tendency`, `skin_layers`, feature/BC grading, `p_elevate`, adaptation passes, `eta_target`, and LEB waves, respecting mesher constraints.
3. 6 sequential proposals selected by an action-conditioned surrogate after the first 18 outcomes, balancing predicted Pareto improvement and uncertainty.

This produces **28,800 candidate campaign solves**. Treat infeasible/failed meshers as valuable labels (`failure_type`, time-to-failure), not missing rows.

For every candidate retain:

- displacement L2 and energy-norm errors against reference;
- compliance/strain-energy and loaded-interface displacement error;
- volume-weighted stress-percentile error;
- DOFs, element counts by type/order, mesh/assembly/solve/total seconds, peak memory;
- quality statistics, convergence status, and failure reason.

The learning target should be this vector plus a feasibility probability. At deployment, rank actions under the requested DOF/time budget or derive a utility such as `-log(error) - lambda_D log(DOF) - lambda_T log(seconds)`. Do not permanently bake one choice of `lambda` into the stored label.

## C. Reference truth and local sizing labels

For each of the 1,200 physics problems:

1. Build an independent quadratic-tet reference mesh.
2. Adapt it using ZZ/energy indicators until the last two refinements change compliance, strain energy, and loaded-interface displacement by less than **0.25%**, and the estimated global energy error is below **0.5%**. Quarantine cases that do not enter this regime.
3. On a random 10% audit subset, add one further `h` or `p` level and use Richardson-style QoI extrapolation/effectivity checks. Include manufactured elasticity cases as solver-label unit tests, not ordinary training cases.
4. Save three expert checkpoints (coarse/medium/fine budgets). At roughly 2,000 fixed or Poisson-disk sample points per problem, query the containing expert tetrahedron and store normalized **`log(h*(x)/L)`**. This yields about **2.4 million local sizing samples** without new solves. It exactly matches PolyMesh’s scalar-isotropic capability.
5. Compare candidate fields on reference quadrature points. Avoid raw stress maxima and singular point loads.

## D. Cost on one workstation

These are explicit planning assumptions, not measured PolyMesh timings:

- ordinary candidate configuration, **including meshing and any configured adaptive passes**: mean **2 minutes**;
- converged reference trajectory including its intermediate checkpoints: mean **30 minutes**;
- 20% overhead for failed meshes, retries, I/O, and long-tail cases;
- jobs run serially for solver-hour accounting. Two concurrent jobs may reduce wall time only if RAM permits.

Calculation:

- Candidates: `28,800 x 2 min = 960 solver-hours`.
- References: `1,200 x 30 min = 600 solver-hours`.
- Base total: **1,560 solver-hours**.
- With 20% overhead: **about 1,870 solver-hours**, i.e. **78 serial days** or about **39 wall-days at two truly concurrent jobs**.
- Sensitivity: if ordinary candidates average 5 rather than 2 minutes, the total becomes about **3,600 solver-hours** (150 serial days). The correct order of magnitude is therefore **10^3 solver-hours**, not 10 or 10^5.

First run a **20-geometry pilot** with the identical 6 x 24 design: 2,880 candidates plus 120 references, approximately **190 solver-hours including overhead** under the same assumptions. Use it to measure real solve-time distributions, identify invalid action combinations, and revise only the cost—not the held-out split or label definitions.

Store compact summaries for all candidates and full fields only for references and Pareto winners; a practical storage target is roughly **100–250 GB**, dominated by 1,200 reference/adaptive trajectories.

## What this means for PolyMesh

1. Implement the learning dataset conceptually as **context + action -> measured outcomes**, not context -> one winner.
2. Use a converged reference only once per CAD/BC and amortize it over 24 candidate configurations.
3. Train a separate scalar `log h(x)` head from adaptive expert checkpoints; do not generate anisotropic metric labels until the mesher can consume them.
4. Spend BC diversity on fixture/load topology, direction, and footprint—not redundant force magnitudes in a linear problem.
5. Begin with 1,200 independent physics cases and about 29,000 candidates; literature supports this as a defensible workstation-scale minimum. Grow with active learning only where action uncertainty or held-out family error remains high.
6. Keep real CAD in training and real family-disjoint CAD in test. Synthetic-to-real transfer for this exact task remains an open research question.
