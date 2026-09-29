<!-- Generated 2026-08-09 by a research subagent for the variable-everything + advisor program. Raw, unedited. -->

# Prior art: ML for mesh generation and adaptive refinement

# Prior art: ML for mesh generation and adaptive refinement

## Bottom line

**Yes, important pieces have been done, but not the full PolyMesh problem.** Published systems have learned (i) a scalar local size field, including a 3D tetrahedral linear-elasticity version; (ii) cell marking/refine/coarsen policies; (iii) local or goal-oriented error indicators; and (iv) in a few CFD/cloth papers, anisotropic metric tensors. The strongest results are credible demonstrations on narrow, parameterized PDE families. I found no published or commercial system that jointly chooses **global mesh size, mesher/element family, shape mix, grading, skin layers, polynomial order, and adaptive schedule** from a general CAD solid plus structural BCs, optimizing measured accuracy per DOF or second. That combined advisor remains novel.

## 1. MeshingNet and MeshingNet3D — the closest direct analogue

### MeshingNet (2D, 2020)

Primary source: [paper/arXiv](https://arxiv.org/abs/2004.07016), [PDF](https://arxiv.org/pdf/2004.07016).

**What it learns.** A pointwise map

\[
F(\Gamma,B,M,x)\to A(x),
\]

where \(A(x)\) is a scalar maximum triangle area. Triangle remains the actual mesher. Thus this is not neural element connectivity generation and not tensor/an-isotropic sizing; it is a learned scalar sizing field supplied to a conventional Delaunay mesher.

**Featurization.** Geometry is a bounded 6–8 vertex polygon. A query point is represented by mean-value coordinates (MVCs), providing translation/rotation invariance relative to the polygon. Inputs concatenate polygon vertex coordinates, the point MVCs, encoded BC/load magnitudes, and material/PDE parameters. The linear-elasticity example has 27 inputs: 16 polygon-coordinate values, 8 MVCs, traction magnitude, density, and Poisson ratio. A separate model/norm is required for each PDE family and chosen error norm.

**Architecture/training.** They compare an FCN and two residual variants. The FCN dimensions are `X-32-64-128-128-64-32-8-1`, ReLU hidden activations; ResNet1 adds a long skip and ResNet2 multiple skips. Adam, MSE, batch 128, Keras/TensorFlow. They sample 3,800 problems (3,000 train/800 test), with roughly 1,000 coarse elements each, hence more than three million pointwise pairs.

**Labels.** For every training problem they solve on a low-density uniform mesh (LAS) and a much finer uniform mesh treated as the high-accuracy solution (HAS). The LAS is interpolated to the fine mesh, local error is evaluated and projected back, and the target is \(A(x_i)=K/E(x_i)^\alpha\), with \(\alpha=1\); \(K\) sets the requested total element count. This is supervised imitation of an offline high-fidelity/error-estimation process, not end-to-end optimization of wall time.

**Reported performance.** On unseen 2D Poisson octagons, at 4,000 elements, uniform-mesh relative \(L_1\) errors lie about 0.0015–0.0025 while MeshingNet errors lie about 0.0007–0.0015. On 2D plane-stress problems, mean potential energies are −7.6813/−7.6815/−7.6816 for FCN/ResNet1/ResNet2, closer to the fine-mesh baseline −7.7293 than uniform meshes (−7.6030), and reported better than one-pass ZZ meshes at equal element count. Predicting all coarse-cell sizes takes 0.046 s, reported **>300× faster** than the offline a-posteriori procedure. Training each model took 134–142 minutes on a Tesla K40c.

**Limitations/generalization.** “Arbitrary geometry” means new members of a tightly bounded low-vertex polygon distribution, not arbitrary CAD topology. BC placement is strongly templated. The labels compare coarse and fine uniform solutions and the output is scalar isotropic size. It gives an improved initial mesh but cannot certify a requested error; the authors explicitly recommend retaining a conventional posterior error pass when certification matters.

**Code/weights.** I found no official public repository or pretrained weights. The paper describes TensorFlow/Keras, Triangle, and an FE solver but does not release an implementation.

### MeshingNet3D (2021)

Primary source: [journal record/DOI](https://doi.org/10.1016/j.advengsoft.2021.103021); the most detailed accessible technical description is Zhang’s [2023 Leeds PhD thesis](https://etheses.whiterose.ac.uk/id/eprint/34981/1/Zhang_Z_Computing_PhD_2024.pdf).

**Method.** The same pointwise concept becomes

\[
F(G,B,M,x)\to S(x),
\]

where \(S\) is an **isotropic** target tetrahedral size (e.g. average edge length). TetGen consumes a background tetrahedral mesh plus `.mtr` vertex sizes; FreeFem++ solves linear tetrahedral elasticity. Query positions use 2.5D MVCs for prismatic geometries or full 3D MVCs for polyhedra. A coarse solve plus ZZ energy estimator generates \(S(x)=K/E(x)\). Importantly, training points are sampled from an already nonuniform adapted mesh (10% of elements), weighting examples toward error-critical regions.

**Architecture/data.** Fully connected networks, typically about six hidden layers, increasing then decreasing width, ReLU hidden/linear output, Adam/MSE/batch 128. One clamped-beam model uses 32-64-128-64-32-8 hidden units and 10.74 million point samples from 3,000 problems; a layered-material model has 19.72 million samples. Training is reported as no more than 3 h on an RTX 2070. Four parameterized 3D linear-elasticity families test geometry, load/fixture, and material changes; 500 unseen problems per experiment are compared with equal-size uniform and ZZ-target (“ground truth”) meshes.

**Reported performance.** The paper/thesis reports NN-guided meshes generally on par with the ZZ-guided targets in FE energy and substantially better than equal-size uniform tetrahedral meshes. The figures are mostly histograms rather than a single universal percent saving. This is strong evidence that scalar size-field imitation works within each parameterized family, not evidence of general CAD transfer.

**Code/weights.** No official public code or weights located. TetGen and FreeFem++ are public, but the training/pipeline implementation is not.

**What this means for PolyMesh.** Reproduce the **idea**, not its coordinate encoding: use PolyMesh’s campaign solver to produce targets and learn a scalar \(h(x)\) with a CAD/BC-aware representation. It maps directly to refinement seeds and is feasible on the same RTX 2070. Treat it as a warm-start for the existing ZZ/adaptive loop. It does **not** address mesher choice, element family/mix, \(p\), or anisotropy, and MVCs will not scale naturally to arbitrary BREP topology.

## 2. Reinforcement learning for AMR

### Yang et al., “Reinforcement Learning for Adaptive Mesh Refinement” (2021/2023)

Primary source: [arXiv 2103.01342](https://arxiv.org/abs/2103.01342), [PDF](https://arxiv.org/pdf/2103.01342); project page: [RL for AMR](https://sites.google.com/view/rl-for-amr/).

**MDP.** The state is variable-size: one observation per element. Each observation is a local 24×24 sampled window (16 samples across the element plus 4 context samples per side), with two channels: numerical solution and refinement depth. The action is global and sequential: choose one element to refine, or no-op. Reward with known truth is normalized reduction in global \(L_2\) error; without truth it uses the solution difference with versus without the proposed refinement. The policy therefore optimizes a sequence rather than a greedy instantaneous indicator.

**Networks.** IPN independently scores each element with a CNN/MLP and a global softmax; Graphnet adds mesh-adjacency message passing; Hypernet generates scoring-network parameters from the global state. They train with REINFORCE or PPO in MFEM through `MFEMCtrl`.

**Results.** Across static functions, advection, and Burgers tests, RL matched or beat the greedy ZZ baseline in **20 of 24** reported cases, sometimes even beating the “refine largest true-error cell” greedy oracle because the oracle is myopic. Policies trained on 8×8 meshes were deployed up to 200×200 (625× more elements) when the local solution-to-mesh scale was preserved; budget transfer from 10/20 actions to 50/100 was also demonstrated. However, transfer degraded when local length scales changed: IPN/Graphnet trained on circles underperformed ZZ on some 16×16 tests, and Hypernet sometimes collapsed to no refinement. Training took roughly 6–18 CPU-hours; inference runtime was the same order as ZZ, though Graphnet became much slower on the largest example.

**Code/weights.** MFEM is public, but I found no maintained official repository/weights for this exact 2021 system; the project page exposes media and paper material.

### Foucart, Charous & Lermusiaux, “Deep Reinforcement Learning for AMR” (2023)

The third author is **Pierre F. J. Lermusiaux**, not Willcox. Primary source: [arXiv 2209.12351](https://arxiv.org/abs/2209.12351), [JCP DOI](https://doi.org/10.1016/j.jcp.2023.112251).

**POMDP.** A local agent visits one cell at a time. Observation includes the integrated jump/nonconformity on the cell and neighbors, global mean jump, current resource fraction \(p\), and optional local physics features. Actions are `{coarsen, do nothing, refine}`. The reward is signed logarithmic solution change (reward refinement that materially changes the solution; penalize harmful coarsening) minus a resource penalty using an asymptotic barrier \(B(p)=p/(1-p)\). Crucially, it needs neither exact solutions nor a precomputed label set: every training action re-solves and measures change. DQN/A2C/PPO use two 64-neuron hidden layers.

**Results.** Policies trained cheaply on small problems were competitive with Kelly/gradient heuristics over 1D/2D steady and unsteady advection, Poisson/advection-diffusion, DG and HDG. The paper reports the same accuracy with fewer DOFs in several tests and one training-regime comparison where random-mesh initialization used about **half the elements** of coarse-only initialization for the same accuracy. It does not provide one aggregate DOF-saving percentage against the heuristics. Training is about 10^5 environment steps and 1–3 CPU-hours. Generalization covers changed forcing/BCs, larger budgets/problem sizes, and related PDE/discretization classes, but demonstrations remain small structured quad/hex families.

**Code/weights.** deal.II, Gym, and Stable-Baselines3 are public; no official experiment repository or pretrained policy was located.

### Gillette, Keith & Petrides, “Learning Robust Marking Policies” (SIAM SISC 2024)

Primary source: [arXiv 2207.06339](https://arxiv.org/abs/2207.06339), [SIAM DOI](https://doi.org/10.1137/22M1510613), code capsule [Code Ocean DOI](https://doi.org/10.24433/CO.0890995.v1).

**MDP.** This is materially simpler and more transferable than cellwise RL: retain the proven estimator and learn only its global marking parameters. A fixed-size observation contains progress toward the target/budget plus normalized statistics (log RMS and standard deviation) of local error estimates. For \(h\)-AMR the continuous action is marking threshold \(\theta\in[0,1]\). For \(hp\)-AMR the action is \((\theta,\rho)\): high-error cells receive \(h\), the next band \(p\). PPO trains a projected-Gaussian policy represented by two 128-unit Swish layers. Reward/terminal objective is either cumulative DOFs to reach target error or final error under a cumulative-DOF budget.

**Results.** On the L-shaped Poisson benchmark, the learned dynamic \(\theta\) reached target error using **18–61% as many cumulative DOFs** as fixed-threshold policies; median fixed-threshold cost was about 2× the learned policy. An \(hp\) policy trained only on 2D single-reentrant-corner problems generally beat the best fixed \((\theta,\rho)\) on unseen domains/PDEs and transferred without retraining to a 3D Fichera corner, improving final error by a factor **1.47** at the same budget. It did lose narrowly on one training geometry and the unseen star domain—useful evidence that transfer is good, not universal.

**Code/weights.** Reproducible Code Ocean capsule is public; implementation uses MFEM, PyMFEM and RLlib.

### Yang et al., multi-agent AMR (2023)

Primary source: [arXiv 2211.00801](https://arxiv.org/abs/2211.00801); official code: [LLNL/marl-amr](https://github.com/LLNL/marl-amr).

Each element is an agent with `{no-op, refine, de-refine}`. Node observation is log local error plus one-hot depth; edges encode relative depth and feature advection direction. A Value-Decomposition Graph Network with graph attention uses centralized training/global reward but decentralized element decisions, allowing simultaneous actions and anticipatory refinement. On time-dependent 2D advection, its normalized efficiency was **1.05–1.70×** the best swept threshold policy across in-distribution and OOD tests (longer time, triangular mesh, anisotropic/ring/opposite-moving features, star geometry). This is the most convincing public implementation of anticipatory AMR, but it is still isotropic \(h\)-refinement on pedagogical advection, trained with local true/reference error.

**What this means for PolyMesh.** Start with the Gillette-style **policy over existing robust controls** (`eta_target`, `adapt_passes`, `adapt_leb_waves`, and possibly `p_elevate`) rather than replacing ZZ cell indicators with deep RL. It has a small fixed action/state, low deployment cost, and unusually good geometry/dimension transfer evidence. Cellwise or multi-agent RL becomes worthwhile only if anticipatory time-dependent refinement is later required; for current linear elastostatics, there is little future dynamics to anticipate and supervised campaign optimization is cheaper and more stable.

## 3. GNN/neural error estimators and surrogate indicators

### MeshGraphNets (DeepMind, 2020/2021)

Primary source: [paper](https://arxiv.org/abs/2010.03409); official code/data: [DeepMind repository](https://github.com/google-deepmind/deepmind-research/tree/master/meshgraphnets); project/videos: [site](https://sites.google.com/view/meshgraphnets).

MeshGraphNets is primarily a **learned time-stepper/field surrogate**, not an FEA error estimator. It encodes mesh nodes/edges, runs repeated graph message passing, and decodes per-node state updates. On dynamic cloth it additionally predicts a per-node 2×2 SPD sizing tensor; a generic local remesher splits, collapses, and flips edges using that tensor. If target sizing is unavailable, it estimates tensor labels from consecutive ground-truth meshes. Thus it really does feed a remesher—but only in the dynamic-cloth experiments, jointly with a learned physics rollout, not as accuracy-certified AMR for a conventional elasticity solver.

The learned remesher was approximately on par with running the learned dynamics on ground-truth mesh sequences; the complete dynamic-flag learned step was 837 ms versus 26,199 ms for the source simulator (about 31×), but that number conflates neural physics replacement with remeshing. It transferred from rectangular flags to fish-shaped flags and a windsock. The paper also reports rollout decoherence after about 50 flag steps and larger full-trajectory error with scale: a clear warning against treating a visually plausible GNN rollout as a reliable FEA answer.

### E2N: Error Estimation Networks for Goal-Oriented Mesh Adaptation

Primary source: [arXiv 2207.11233](https://arxiv.org/abs/2207.11233); archived dependency stack: [Zenodo 6722155](https://zenodo.org/record/6722155).

E2N replaces only the expensive dual-weighted-residual enrichment/error-estimation stage. A small elementwise FC network (one hidden layer) maps local element geometry, physical coefficients, forward/adjoint values, recovered gradients/Hessians, and coarse error approximations to one signed element QoI-error contribution. Training labels come from standard enriched goal-oriented solves over 100 randomly parameterized 2D tidal-turbine shallow-water cases. Predicted indicators are then used in a real anisotropic Riemannian-metric workflow and Mmg mesh adaptation.

On unseen cases, standard goal-oriented and E2N adaptation both reduced QoI error by roughly **two orders of magnitude** versus uniform refinement at a given DOF. At comparable QoI errors (~0.039% aligned/~0.037% offset), E2N approximately **halved total runtime**; the error-estimation component alone was **90% cheaper**. It also worked on reversed flow and parameters/bathymetry outside training bounds, but the authors explicitly limit claims to related shallow-water physics and note untested BC formulations/fields. This is one of the clearest examples that a learned indicator actually drives a production-style mesher rather than merely predicting a field.

**Code/weights.** The exact software stack is archived; a standalone maintained E2N package or downloadable pretrained weights was not located.

### Which methods really drive meshes?

- **Do:** MeshingNet/3D (scalar field → Triangle/TetGen), E2N (indicator → tensor construction → Mmg), Fidkowski–Chen below (learned tensor → BAMG), MeshGraphNets dynamic cloth (tensor → local remesher), and the RL AMR papers (mark/refine/coarsen in the solver loop).
- **Usually do not:** generic neural PDE surrogates, GNN field predictors, and many “learned error” papers that report correlation/RMSE only. A learned solution or error heatmap is not a meshing result until a conforming mesh is generated and the original PDE is re-solved on it.

**What this means for PolyMesh.** A surgical estimator surrogate is safer than replacing the solver. Train against PolyMesh’s ZZ/reference results, use the neural output only to propose seeds/marking, then re-solve and retain the existing estimator as guardrail. MeshGraphNets is useful architectural prior art for local graph encoders, not proof that a structural mesh advisor will generalize.

## 4. Learned anisotropic metric fields

### Fidkowski & Chen, metric-based goal-oriented ML adaptation (JCP 2021)

Primary source: [author PDF](https://public.websites.umich.edu/~kfid/MYPUBS/Fidkowski_Chen_2020_JCP.pdf), [DOI](https://doi.org/10.1016/j.jcp.2020.109957).

This is the clearest direct precedent. A network predicts **normalized SPD metric anisotropy**—stretch magnitude and direction—while an adjoint-weighted residual determines absolute size. Inputs are normalized/log-metric encodings of Hessians of every primal and adjoint state component (4s scalar features in 2D), optionally plus per-equation relative error contributions. Output is the two independent components of a trace-free log metric; exponentiating guarantees an SPD tensor. One- or two-hidden-layer MLPs are trained on 121,840 element samples from MOESS-optimized 2D RANS-SA meshes (flat plate and several airfoils, Mach regimes subcritical through supersonic, lift/drag QoIs). BAMG consumes the resulting metric.

Across p=2 and p=3 adaptive sequences, learned metrics generally beat a Mach-Hessian anisotropy heuristic and often matched or exceeded MOESS at equal DOF; on unseen tandem-airfoil geometry and an unseen moment QoI they converged to the reference output in fewer adaptive iterations. The paper attributes occasional improvement over its teacher to small-network regularization. Results are mostly convergence curves rather than a single percent-DOF number. Scope is 2D aerodynamic DG/RANS with a primal+adjoint already available; 3D is proposed, not demonstrated.

**Code/weights.** No public implementation or weights located.

### MeshGraphNets tensor sizing

As noted above, MeshGraphNets predicts a full 2×2 tensor \(S_i\) and drives split/collapse/anisotropic-Delaunay flips. This is genuine learned metric remeshing, but demonstrated for 2D manifold meshes embedded in 3D (cloth), not volume tetra/hex FEA.

### Near-optimal CFD spacing papers are often still scalar

Sanchez-Gamero, Hassan & Sevilla, [arXiv 2406.16057](https://arxiv.org/abs/2406.16057), learns near-optimal background-mesh spacing for unseen RANS operating conditions/CAD airfoil geometries and discusses highly stretched boundary-layer meshes. However, the ANN output itself is nodal **scalar spacing**; the inflation layer is generated separately. It should not be cited as direct tensor prediction.

**What this means for PolyMesh.** This line cannot be implemented faithfully today: PolyMesh accepts scalar isotropic \(h(x)\), so a predicted SPD metric would be discarded. First add a metric-aware mesher/adaptor contract (at minimum symmetric 3×3 SPD tensors with eigenvalue/aspect-ratio clamps and boundary-layer handling). Until then, learn scalar density and shape/mesher choices. Do not claim anisotropic ML capability merely because meshes contain stretched prisms—the learned output must control orientation-dependent size.

## 5. Commercial/industrial state of the art

The evidence is marketing-level and much less specific than the papers above.

- **Ansys.** Discovery/Mechanical ship extensive rule-based automatic meshing, and Ansys markets AI/ML simulation surrogates and optimization. In 2026 R1, **Mesh Agent** is described as an exploratory AI agent that diagnoses meshing failures and recommends validated remediation steps, not a learned PDE/BC-to-optimal-density policy ([official release](https://news.synopsys.com/2026-03-11-Synopsys-Launches-Ansys-2026-R1-to-Re-Engineer-Engineering-with-Joint-Solutions-and-AI-Powered-Products)). Ansys’s public [AI/ML overview](https://www.ansys.com/blog/how-ai-and-ml-are-changing-simulation) does not disclose a trained metric/element-choice mesher with accuracy-per-DOF benchmarks.
- **Siemens / Altair.** [Simcenter HyperMesh](https://www.siemens.com/en-us/products/simcenter/simulation-modeling-visualization/hypermesh/) combines automated CAD classification/joining/meshing with PhysicsAI surrogate prediction. Public claims do not establish that PhysicsAI selects mesh sizes/types from BCs. Altair HyperMesh’s public AI feature is primarily **shape recognition** to automate model cleanup, feature extraction and workflow operations, plus PhysicsAI field surrogates—not a published learned meshing objective.
- **Cadence.** [Fidelity CFD preprocessing/meshing](https://www.cadence.com/en_US/home/tools/system-analysis/computational-fluid-dynamics/pre-processing-meshing.html) and the broader [CFD page](https://www.cadence.com/en_US/home/tools/system-analysis/computational-fluid-dynamics.html) use “AI-driven methodology” and “intelligent automatic” language; disclosed capabilities include AutoSeal, automated V2S/S2V meshing, viscous layers, and Flashpoint goal-oriented surface meshing. Cadence gives workflow-time claims (days to hours), but no public training data, architecture, learned metric target, solver-error benchmark, code, or weights. Treat it as proprietary automation, not verified MeshingNet-like ML.
- **nTop.** nTop provides configurable, potentially fully automatic implicit-to-analysis meshing and advertises analysis-ready meshes in minutes via ChopMesh ([official overview](https://www.ntop.com/resources/blog/meshing-in-fea-cfd-manufacturing/), [webinar](https://www.ntop.com/resources/webinars/eliminate-the-meshing-bottleneck-from-implicit-to-analysis-ready-in-minutes/)). It does not publicly claim a learned BC/PDE-conditioned sizing or element-choice model.

**Code/weights.** None of these commercial systems exposes meshing training code, datasets, or weights. “Automatic,” “intelligent,” “AI-powered workflow,” geometry recognition, failure-diagnosis agents, and physics surrogates should not be conflated with a learned mesh-parameter optimizer.

**What this means for PolyMesh.** There is room to differentiate on a measurable claim vendors do not substantiate publicly: for a held-out CAD+BC campaign, advisor proposals reduce reference error per DOF/second versus fixed defaults and traditional ZZ warm starts. Publish ablations and failure cases; do not compete on vague “AI meshing” language.

## 6. Code/weights availability at a glance

| Work | Public implementation | Public pretrained weights |
|---|---|---|
| MeshingNet / MeshingNet3D | No official repo located | No |
| Yang 2021 single-agent RL-AMR | Project page; underlying MFEM public, exact maintained repo not located | No |
| Foucart–Charous–Lermusiaux | Underlying deal.II/SB3 public; experiment repo not located | No |
| Gillette–Keith–Petrides | [Reproducible Code Ocean capsule](https://doi.org/10.24433/CO.0890995.v1) | Capsule contains reproducibility artifacts; not a general pretrained product |
| Yang et al. VDGN MARL | [LLNL/marl-amr](https://github.com/LLNL/marl-amr) | Repository artifacts/configs; check release for exact checkpoints |
| MeshGraphNets | [Official DeepMind code/data](https://github.com/google-deepmind/deepmind-research/tree/master/meshgraphnets) | Code/data public; pretrained checkpoint coverage is limited |
| E2N | [Archived stack](https://zenodo.org/record/6722155); no standalone maintained package located | No readily downloadable general weights |
| Fidkowski–Chen learned metric | No public repo located | No |

## Ranked approaches most transferable to PolyMesh

1. **MeshingNet3D-style supervised scalar sizing warm-start.** It is the closest match to linear elasticity, CAD-conditioned BC/material inputs, TetGen-like tetra meshing, the existing scalar refinement-seed interface, and the available RTX 2070. PolyMesh can improve the labels by optimizing actual reference error per DOF/second rather than merely imitating \(K/E\). Keep ZZ afterward for safety.
2. **Gillette-style RL/contextual policy over global marking and hp controls.** Learn a very small policy over `eta_target`, `adapt_passes`, `p_elevate`, and `adapt_leb_waves` using normalized error-distribution and budget statistics. It preserves PolyMesh’s trusted estimator and has the strongest evidence of 2D→3D geometry transfer. For the first implementation, a contextual bandit or supervised ranking model may capture most of the value with less training variance than PPO.
3. **Fidkowski–Chen normalized metric prediction, after anisotropic infrastructure exists.** It is the strongest precedent for a learned tensor field that actually drives a mesher and improves goal accuracy at fixed DOF. It could be highly valuable around stress concentrations, thin features and boundary layers, but it is blocked by PolyMesh’s scalar-only sizing contract.

## Explicit generalization warnings

- **Local-distribution shift is the recurring failure mode.** Yang’s policies scaled 625× only when local solution/mesh length scales were preserved; changing that scale degraded or reversed gains.
- **Global CAD novelty is not established by MeshingNet.** Its “unseen geometries” are samples from a fixed, low-dimensional polygon/prism family with templated BCs. A bracket, casting, thin shell-like solid, and multi-body assembly are not interpolation within that family.
- **RL is not automatically better than a good estimator.** Hypernetwork policies sometimes learned no refinement; cellwise RL needs many costly solver interactions; and several papers report qualitative/plot-level rather than universal DOF savings. Always benchmark against tuned ZZ/Dörfler and random/search baselines.
- **Goal-oriented models bind to a QoI and physics family.** E2N/Fidkowski transfer via normalized adjoint features, but neither proves transfer from CFD/shallow water to structural energy, peak stress, or compliance without retraining.
- **Neural physics rollout quality is not mesh quality.** MeshGraphNets’ speedups replace the solver and can decohere over long rollouts. PolyMesh should always evaluate candidate meshes using its real FE solver.
- **A scalar field cannot deliver anisotropy.** Neither MeshingNet3D nor scalar “near-optimal spacing” papers solve tensor sizing. Until the mesher consumes a 3×3 SPD metric, this remains a hard product limitation.
- **Joint element-type/mesher selection is largely unstudied.** The cited systems keep the mesher and element family fixed. PolyMesh’s categorical/continuous hybrid action space is therefore both an opportunity and a source of extrapolation risk; use uncertainty/OOD rejection and fall back to trusted defaults plus ZZ when a query lies outside campaign coverage.
