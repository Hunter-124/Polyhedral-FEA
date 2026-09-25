# Architecture decision records

ADR bodies are frozen once accepted. A later ADR amends or supersedes an earlier one; the earlier file is never rewritten. Current project status: [docs/STATUS.md](../STATUS.md).

## Geometry / CAD

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0001](0001-geometry-kernel.md) | Geometry kernel | OpenCASCADE for B-rep/STEP behind `POLYMESH_WITH_OCC`; STL for comparison/legacy | accepted; amended by 0020 |
| [0020](0020-brep-volume-meshing.md) | True BRep volume meshing | Raw CAD (`.brep`/`.step`/`.stp`) is the product input, not hand-authored STL | accepted; supersedes 0001 STL-first default in part |
| [0035](0035-boundary-conformity.md) | Boundary nodes belong on the BRep | Boundary nodes are placed on the B-rep, not near it | accepted; supersedes the snap half of 0015 |
| [0039](0039-a-stranded-boundary-node-is-rescued.md) | A stranded boundary node is rescued | Rescue stranded boundary nodes instead of leaving them off the surface (closes the 0035 §6 cone residual) | accepted |

## Mesher

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0004](0004-mesh-data-structure.md) | Mesh data structure | Face-based owner/neighbour mesh (OpenFOAM-style) | accepted |
| [0010](0010-mesh-data-structure-edge-vs-face.md) | Edge vs face store | Keep face-based primary topology; edge index optional | accepted |
| [0012](0012-hybrid-graded-tet.md) | Hybrid meshing | Graded all-tet fill plus a conforming mixed-type zoo (`mixed_fill` / `kHybrid`) | accepted; amended |
| [0013](0013-hex-pyramid-transition.md) | Hex core + pyramid skin | Interior hex8, boundary cells split into pyramid5 for conforming transitions | accepted; FE path amended |
| [0015](0015-grid-fill-limits.md) | Cartesian grid-fill limits | Product meshers fill a Cartesian lattice; true Delaunay/frontal deferred | accepted; snap part superseded by 0035 |
| [0018](0018-graded-conformity.md) | Graded tet conformity via LEB | Longest-edge bisection replaces 2:1 hanging Kuhn cubes | accepted |
| [0021](0021-varyhedron-packing.md) | Varyhedron | Variable polyhedral packing mesher driven by CAD edge/face constraints | accepted; ranking refined by 0023 |
| [0025](0025-geogram-cvt-vendor.md) | Vendor Geogram for restricted CVT | Vendor Geogram clipped-Voronoi kernels (BSD-3) | accepted; `cvt_poly` experimental |
| [0030](0030-the-ruler-was-wrong.md) | The ruler was wrong | Retract the fan-transition defect; draw the curvature already computed | accepted |
| [0031](0031-a-jut-has-a-side.md) | A jut has a side | Fix graded-sphere craters by respecting which side a jut is on | accepted |
| [0032](0032-stl-order-determinism.md) | Mesh independent of standard library | Mesh output must not depend on STL container/sort order | accepted; amended (unstable sort) |
| [0036](0036-a-symmetric-part-gets-a-symmetric-tiling.md) | Symmetric tiling | A symmetric part gets a symmetric tiling (canonical visiting order) | accepted |

## Solver / FEA

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0003](0003-element-formulations.md) | Element formulations | One `fea::Element` trait: isoparametric FEM p=1..4 plus VEM | accepted; p-mechanism refined by 0019 |
| [0005](0005-benchmark-baseline.md) | Benchmark baseline | Own uniform tet10 path as baseline; CalculiX audit cross-check | accepted |
| [0008](0008-cuda-backend.md) | CUDA backend | Optional first-class CUDA backend (`POLYMESH_WITH_CUDA`) with runtime dispatch | accepted |
| [0009](0009-tier1-verification-setups.md) | Tier-1 verification setups | How Kirsch / Goodier / L-domain closed-form cases are posed | accepted |
| [0011](0011-vem-k1.md) | VEM k=1 | VEM k=1 for convex polyhedra as `kPolyVem` | accepted |
| [0017](0017-vem-k2.md) | VEM k=2 | VEM k=2 with serendipity edge-midpoint DOFs | accepted |
| [0019](0019-mixed-fe-vem-adaptive-order-core.md) | Mixed FE+VEM adaptive-order core | Arbitrary-p hierarchical basis; h, p and shape chosen per region | accepted |
| [0038](0038-a-fixture-is-applied-to-the-boundary.md) | Fixtures on the boundary | Dirichlet fixtures constrain boundary nodes, not a volume of nodes | accepted; §6 superseded by 0041 |
| [0040](0040-a-boundary-condition-is-the-exact-closure-of-a-cad-face.md) | BC names a CAD face closure | A boundary condition applies to the exact closure of a CAD face | accepted |
| [0044](0044-glm-cannot-be-the-math-library.md) | GLM cannot be the math library | Keep Eigen as the single math library; reject GLM | accepted |

## Adaptivity / sizing

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0014](0014-adapt-seed-remesh.md) | Dörfler seed remesh | ZZ recovery, Dörfler marking, graded global remesh around seeds | accepted |
| [0016](0016-local-h-refine.md) | Local h-refine | Local longest-edge bisection with no hanging nodes | accepted |
| [0026](0026-anisotropic-metric-adaptivity.md) | Anisotropic metric adaptivity | Real metric-field size field; shipped as `adapt::Metric3d` / `adapt::MetricGrid` | accepted |
| [0034](0034-spectral-sizing-and-coarsening.md) | Spectral sizing and coarsening | Spectral sizing, budget-feasible advisor, and derefinement | accepted |
| [0037](0037-a-box-selection-is-a-region.md) | A box selection is a region | Box selections are regions; smooth fields are sampled on element sizes | accepted |

## Advisor

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0024](0024-advisor-measure-answers.md) | Advisor measure-first answers | Normative Q&A from the post-M1–M4 review; law until measurement overturns it | accepted |
| [0027](0027-learned-mesh-advisor.md) | Learned mesh advisor | Action-conditioned outcome model, not a winner classifier | accepted; §6–§7 reversed in practice |

## GUI / presentation

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0006](0006-gui-after-solver-core.md) | GUI | GLFW + Dear ImGui + OpenGL GUI, pulled forward alongside the solver | accepted; revised, restyled |
| [0041](0041-a-deformed-render-carries-its-undeformed-outline.md) | Deformed render outline | Deformed renders carry the undeformed outline | accepted |
| [0042](0042-the-advisor-explains-itself-on-screen.md) | The advisor explains itself on screen | GUI film showing advisor decisions and solved fields | accepted; revised by 0043 |
| [0043](0043-a-film-someone-can-read.md) | A film someone can read | Pacing and `icecream_cone` default for the film | accepted; supersedes 0042 §6 |

## Process / project

| ADR | Title | Decision | Status |
|---|---|---|---|
| [0002](0002-license-bsd3.md) | License | BSD-3-Clause | accepted; supersedes AGPL-3.0-or-later |
| [0007](0007-language-cpp.md) | Implementation language | C++20, CMake + Ninja | accepted |
| [0022](0022-experiment-warehouse-grok-loop.md) | Experiment warehouse + Grok loop | Full experiment warehouse in git; headless Grok improvement loop | accepted; loop part retired (see Notes) |
| [0023](0023-measure-first-tet-primary-cvt-path.md) | Measure-first, tet primary, CVT | Measure before claiming; tet primary; restricted CVT ranked packing path | accepted; supersedes 0021 ranking in part |
| [0028](0028-boundary-conformance-hardening.md) | Boundary-conformance hardening | Harden projection, probes and evidence paths across mesh/solve | accepted; complete |
| [0029](0029-independent-truth-and-honest-gates.md) | Independent truth, honest gates | Independent references, mesh-independent load, gates that cannot fake success | accepted; complete |
| [0033](0033-a-gate-must-measure-what-ships.md) | A gate must measure what ships | Quality gates measure the cell that ships | accepted |

## Notes

- ADR-0022 recorded two things. The **experiment warehouse** remains in force. The **headless Grok improvement loop** is retired: its tooling has been removed from the repo. The ADR body is left unchanged as history.
- ADR-0021 (Varyhedron, variable polyhedral packing) is a core, active product direction, not an experiment. ADR-0023 reordered its algorithm ranking; it did not retire it.
- Frozen ADRs keep their original links. Targets that moved to [docs/archive/](../archive/README.md): `docs/research/**` is now `docs/archive/research/**`; `docs/plans/variable-everything-and-advisor.md` and `docs/plans/mesher-solver-overhaul.md` are now under `docs/archive/plans/`. This affects ADR-0018, 0025, 0026 and 0027.
