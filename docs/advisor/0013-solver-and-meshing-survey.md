# Solver and meshing research survey

This note records the external methods reviewed for the portable-cost advisor
retrain. It separates ideas used now from ideas that need a larger solver or
corpus. The shipped finite-element numerics remain double precision; none of the
training-precision experiments changes the solver.

## Adopted now

### Symbolic sparse-factor cost

The direct-solve label uses the same structural problem Eigen sees after fixed
DOFs and linear constraints are eliminated:

1. build the symmetric reduced sparsity pattern;
2. apply Eigen's default AMD ordering;
3. build the elimination forest with Liu's path-compressed algorithm;
4. run Gilbert-Ng-Peyton column counting without allocating the numeric factor.

For a unit-lower LDLT factor, `factor_nnz` is the number of stored strict-lower
entries. The numeric factor work is reported as the Cholesky column sum
$\sum_j (|L_j|+1)^2$; the two triangular solves add `4 * factor_nnz` FLOPs.
The implementation is checked against brute-force symbolic elimination and
Eigen's actual `SimplicialLDLT::matrixL().nonZeros()` in the direct regime.

Primary sources:

- J. W. H. Liu, “The role of elimination trees in sparse factorization,”
  *SIAM Journal on Matrix Analysis and Applications* 11(1), 1990,
  [doi:10.1137/0611001](https://doi.org/10.1137/0611001).
- J. R. Gilbert, E. G. Ng, and B. W. Peyton, “An efficient algorithm to compute
  row and column counts for sparse Cholesky factorization,” *SIAM Journal on
  Matrix Analysis and Applications* 15(4), 1994,
  [doi:10.1137/S089547989223754X](https://doi.org/10.1137/S089547989223754X).
- T. A. Davis, *Direct Methods for Sparse Linear Systems*, SIAM, 2006,
  [doi:10.1137/1.9780898718881](https://doi.org/10.1137/1.9780898718881).

### Iterative-solve work labels

A CG label is iteration count times a pattern-only per-iteration model:

- sparse matrix-vector multiply: `2 * nnz(K)` FLOPs;
- incomplete-factor triangular applications: `2 * nnz(L_ichol)` FLOPs;
- vector operations and reductions: `10 * nfree` FLOPs;
- compulsory CSR/vector traffic: `12 * nnz(K) + 48 * nfree` bytes.

The iteration count is the same count delivered by the solver progress callback,
not wall time. This follows the standard sparse-CG operation decomposition in
Y. Saad, *Iterative Methods for Sparse Linear Systems*, 2nd ed., SIAM, 2003,
[doi:10.1137/1.9780898718003](https://doi.org/10.1137/1.9780898718003).

### Roofline reporting, not training

The network predicts portable FLOPs and bytes. A committed host calibration
converts them to an optional report-time estimate:

$$t_{solve}=\max(F/P_{host},\ B/W_{host}).$$

This is the roofline lower-bound model of Williams, Waterman, and Patterson,
“Roofline: an insightful visual performance model for multicore architectures,”
*Communications of the ACM* 52(4), 2009,
[doi:10.1145/1498765.1498785](https://doi.org/10.1145/1498765.1498785).
The conversion is never a label: changing hosts must not require retraining.
Mesh work is similarly normalized by a five-run median reference mesh.

### Recovery-based accuracy evidence

The existing ZZ recovery/error path remains the observable adaptation signal.
The retrain does not replace it with an internal solver answer. The method is
from O. C. Zienkiewicz and J. Z. Zhu, “The superconvergent patch recovery and
adaptive finite element refinement,” *Computer Methods in Applied Mechanics and
Engineering* 101, 1992,
[doi:10.1016/0045-7825(92)90023-D](https://doi.org/10.1016/0045-7825(92)90023-D).

## Watch for the next solver cycle

### Algebraic multigrid

AMG would change both the cost model and the useful action space: setup work,
operator complexity, cycle counts, and coarse-grid memory become first-class
labels. Hypre's BoomerAMG is the practical reference implementation:

- V. E. Henson and U. M. Yang, “BoomerAMG: a parallel algebraic multigrid solver
  and preconditioner,” *Applied Numerical Mathematics* 41, 2002,
  [doi:10.1016/S0168-9274(01)00115-5](https://doi.org/10.1016/S0168-9274(01)00115-5).

Adoption is deferred because the current product solver exposes LDLT and
CG/preconditioner cascades, not AMG setup/cycle telemetry. Inventing an AMG head
before the engine can produce those observations would train a label with no
consumer.

### Partitioning and parallel sparse solves

ParMETIS and SCOTCH provide graph/hypergraph partitioning and fill-reducing
orderings for distributed meshes and factors:

- G. Karypis and V. Kumar, “A parallel algorithm for multilevel graph
  partitioning and sparse matrix ordering,” *Journal of Parallel and
  Distributed Computing* 48, 1998,
  [doi:10.1006/jpdc.1997.1403](https://doi.org/10.1006/jpdc.1997.1403).
- F. Pellegrini and J. Roman, “SCOTCH: a software package for static mapping by
  dual recursive bipartitioning,” *HPCN Europe*, 1996,
  [doi:10.1007/3-540-61142-8_588](https://doi.org/10.1007/3-540-61142-8_588).

The present corpus is single-host and the deployed Eigen LDLT ordering is AMD.
Partition features would be misleading until the assembly and solve are truly
distributed.

### Dynamic AMR forests

`p4est` demonstrates scalable forest-of-octrees refinement and repartitioning:

- C. Burstedde, L. C. Wilcox, and O. Ghattas, “p4est: scalable algorithms for
  parallel adaptive mesh refinement on forests of octrees,” *SIAM Journal on
  Scientific Computing* 33(3), 2011,
  [doi:10.1137/100791634](https://doi.org/10.1137/100791634).

Its balance/repartition costs and hanging-node constraints are useful future
advisor targets. They are not added to a corpus whose current adaptation driver
uses local tetrahedral refinement and p-elevation.

### Mesh-quality optimization

Mesquite's target-matrix framework is a useful post-mesh optimization reference:

- M. L. Brewer et al., “The Mesquite mesh quality improvement toolkit,”
  *12th International Meshing Roundtable*, 2003,
  [Sandia report SAND2003-3030C](https://www.osti.gov/biblio/918380).

The current action space selects mesher, size, grading, adaptation, and order.
A smoothing/optimization action should be added only with paired evidence that
quality improves without erasing B-rep fidelity or changing deterministic
node/element identities.

## Meshing baseline and truth independence

Gmsh 4.13.1 is used only in the independent truth chain; it never supplies a
PolyMesh training action. The methodology follows C. Geuzaine and J.-F.
Remacle, “Gmsh: a three-dimensional finite element mesh generator with built-in
pre- and post-processing facilities,” *International Journal for Numerical
Methods in Engineering* 79, 2009,
[doi:10.1002/nme.2579](https://doi.org/10.1002/nme.2579).
Gmsh reads each generated STEP directly, creates quadratic tetrahedra, and
CalculiX 2.23 solves them. New scored truths use strain energy and displacement
probes only. Internal PolyMesh results, max/peak von Mises stress, and
cross-tool meshes are excluded from truth promotion.

## Deferred and rejected ideas

- **Wall-time regression as a primary target — rejected.** It entangles host,
  cache, contention, and build flags. FLOPs, bytes, and dimensionless mesh work
  retain the physical scaling and can be recalibrated.
- **Peak-stress truth — rejected.** Pointwise maxima are singularity- and
  mesh-policy-sensitive; strain energy and displacement converge materially
  better for corpus scoring.
- **Public CAD as supervised FEA rows — deferred.** ABC, Fusion 360 Gallery,
  Thingi10K, MCB, and SimJEB either lack permissive redistribution, explicit
  load/BC/material metadata, or independently reproducible truths. They remain
  geometry-diversity candidates only; no unlabeled public geometry was silently
  promoted into this supervised corpus.
- **INT4/INT8 integer training — rejected for this GPU/toolchain.** Ampere
  supports fast low-bit forward GEMMs, not stable native low-bit backward and
  optimizer kernels in this PyTorch path. The measured QAT prototype keeps
  FP16/BF16 backward state and was slower than shared-trunk TF32. TF32 is the
  measured production training mode; deployed inference stays deterministic
  CPU FP32.
