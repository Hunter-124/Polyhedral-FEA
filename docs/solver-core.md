# The adaptive solver core, explained

This is the human's guide to how PolyMesh decides *what kind of element to put
where* and *how accurately to solve it*. It is written to be read start to
finish by an engineer who has never seen the code. The formal decisions live
in the ADRs (linked inline); this page is the intuition and the map.

The one-sentence version: **a good mesh spends effort where the answer is
hard and saves it where the answer is easy — in three independent currencies
at once: element size (h), polynomial order (p), and element shape.** Getting
all three to cooperate, in one part, without wrecking the solve time, is the
whole game.

---

## 1. Why three knobs instead of one

Every finite-element error comes from two places: the mesh not following the
*geometry* (a faceted circle is wrong no matter how clever the math on each
facet), and the elements not following the *solution* (a linear element can't
represent a curving stress field). These two failures want different fixes.

- **Geometry error** is fixed by **smaller elements (h)**. Polynomial order
  cannot rescue a boundary the mesh has already approximated as flat facets —
  you have to add elements that hug the curve. This is why the curvature
  turning-angle criterion (ADR-0012 amendment) refines *h* near curved
  surfaces and leaves flats alone.
- **Solution error** in a *smooth* region is fixed far more cheaply by
  **higher order (p)**. Doubling the order of an element roughly squares the
  accuracy where the solution is analytic, for a fraction of the DOFs that
  h-refinement would cost. This is the payoff the hierarchical basis unlocks.
- **Shape** is the third currency because different shapes have different
  cost/accuracy per element: a hexahedron carries a given accuracy in far
  fewer DOFs than the tetrahedra filling the same volume, but tets fit awkward
  geometry that hexes cannot. Being able to mix them — and to keep genuinely
  polyhedral cells whole instead of shattering them into slivers — means the
  mesher can pick the cheap shape in the bulk and the flexible shape only
  where it must.

A method with only one knob always overpays. Uniform h-refinement burns
elements on smooth interiors; uniform high order burns DOFs fighting a faceted
boundary; a single element shape is either expensive everywhere or fits
nothing. The core's job is to turn the right knob in the right place.

---

## 2. The hierarchical basis: how p becomes cheap and conforming

(Implementation: `src/fea/hierarchical.hpp`; decision: ADR-0019.)

Classic finite elements attach unknowns to **nodes**: a quadratic tet has ten
nodes, a cubic has twenty, and to raise the order you re-mesh with more nodes.
Mixing orders is painful because a quadratic element and a linear element
side by side do not share a compatible set of nodes on their common face, so
you need extra "constraint equations" to stop the mesh from cracking open
there.

The hierarchical basis attaches unknowns to **entities** — vertices, edges,
faces, and the cell interior — using a special set of functions (integrated
Legendre / Lobatto polynomials). Two properties make everything else work:

1. **Nesting.** The order-*p* function space sits *inside* the order-(*p*+1)
   space. Raising the order only *adds* new functions; it never moves the
   ones already there. A p=1 solution is literally the first slice of the p=2
   solution.
2. **Bubbles vanish on the boundary of their entity.** The order-*k* edge
   function is zero at both ends of its edge; the face functions are zero on
   the face's edges; and so on. So an edge's higher-order detail is owned
   entirely by that edge and shared cleanly by both elements touching it.

Together these give the **minimum rule**, which is the quiet hero of the whole
design: *a shared entity carries the lowest order of the elements touching
it.* If a p=3 hex sits next to a p=1 hex, their shared face is treated as p=1;
the p=3 element simply drops (suppresses) the face and edge detail the p=1
neighbour can't match. Because the bases nest and the bubbles vanish where
they must, the two elements agree exactly along that face **with no constraint
equations at all**. Conformity is free.

We proved this is not just theory: a mesh with a p=1 element next to a p=2
element reproduces a linear displacement field to *zero* error across the
interface (`test_hp_assembly.cpp`), and a smooth manufactured solution
converges in the energy norm at exactly the textbook rate — order 1 for p=1,
order 2 for p=2. When the higher orders (p≥3) come online, the only new work
is a handful of sign flips for edges traversed "backwards" and an orientation
map for faces; the architecture does not change.

### What "order" costs
Higher order adds DOFs per element (edges, then faces, then interior fill in),
so the stiffness matrix gets denser and the solve gets heavier. That is the
price the (h, p, shape) driver weighs against the accuracy the extra order
buys — cheap in smooth regions, wasted against a faceted boundary.

---

## 3. Shape: FE fast paths + VEM for everything else

(Decision: ADR-0019 §1; VEM: ADR-0011 / ADR-0017; hybrid zoo: ADR-0012.)

The solver runs **two formulations that share one global matrix**:

- **Classic finite elements** for the standard zoo — tetrahedra and hexahedra.
  These have exact, quadrature-cheap stiffness and decades of hardening; they
  carry the 90%-plus of the mesh that is well-shaped.
- **The Virtual Element Method (VEM)** for everything else — genuinely
  polyhedral cells, the transition cells between coarse and fine regions, and
  any awkward leftover shape. VEM can integrate a stiffness matrix on a cell
  with any number of faces without ever building shape functions inside it.

The important consequence: the mesher no longer has to **shatter** a
transition polyhedron into a fan of slivers just so a tetrahedral solver can
digest it. Slivers are where accuracy goes to die (a nearly-flat tet has a
terrible stiffness matrix), and they were the source of the bore-hole and rim
artifacts fixed in ADR-0012. Keeping the cell whole and handing it to VEM
removes both the sliver *and* the element-count blow-up of splitting.

"Octahedra, septahedra, and more, all in one part" is exactly this: the FE
tet/hex fast path plus a VEM path that treats every other polyhedron as a
first-class element in the same solve.

### What is implemented (node `fe-vem-assembly`)

- **One assembler.** `fea::assemble_stiffness` already dispatches
  `ElementType::kPolyVem` to `vem_poly_stiffness` and classic zoo elements to
  isoparametric quadrature; both scatter into the same sparse K. Body loads
  follow the same split. No second global system, no mortar.
- **Native-poly transitions.** `mesh::mixed_fill_surface(..., native_poly_transitions=true)`
  emits each 2:1 coarse cell as **one** `MixedCellKind::kPolyVem` with faces
  that match neighbors (single quad vs bulk FE hex, four child quads vs fine
  2×2×2 hex, n-gon with hanging mids on mixed edges). No centroid apex, no
  fan of sliver tets.
- **Product path.** `VolumeMesher::kHybridVem` (CLI `hybridvem`, GUI "hybrid
  VEM") runs that fill, keeps bulk/fine as `kHex8` FE, converts poly cells to
  `kPolyVem`, and skips the hex→pyramid expand used by the default
  `kHybrid` product-FE path.
- **Gate.** Constant-strain patch test with a **linear** displacement
  \(u = Gx\) prescribed on the domain boundary: interior FE and VEM nodes must
  recover \(Gx\) to ~1e-9 m. Covered by `tests/test_fe_vem_assembly.cpp`
  (checkerboard hex FE + hex-as-poly VEM; tet FE + hex VEM; native-poly hybrid
  fill; full pipeline hybrid-VEM mesh).

The default `kHybrid` mesher still uses fan transitions + all-pyramid expand
for the hardened product-FE scorecard. `mesher-tendency` will expose the
fan-split vs native-poly dial for the tuner; until then pick `kHybridVem`
explicitly when you want unsplit VEM transitions.

---

## 4. The driver: choosing (h, p, shape) together

(Implementation target: `src/adapt/`, node `hp-driver`; see ADR-0019 §4.)

The adaptive loop looks at three signals and turns the matching knob:

| Signal | What it measures | Knob it turns |
|---|---|---|
| **Geometry** (a priori) | How sharply the surface curves through a cell (turning angle h·κ) and how thin the wall is | **h** — refine to hug the boundary |
| **Smoothness** (a posteriori) | How fast the error drops when you *try* the next order, from the hierarchical surplus and a ZZ estimate | **p** — raise order where the field is smooth |
| **Cost** (measured) | Predicted DOFs and solve time per choice, calibrated from real campaign data | picks the **shape** and trims choices that cost more than they return |

The cost model is not guessed — it is fitted from the test-lab campaigns
(`bench/campaigns/`, node `feedback-loop`). Every simulation the lab runs
records mesh time, solve time, DOFs, and accuracy against a hand-calculated
truth, tagged by the geometric conditions of the part. Over many runs the
driver learns *which knob actually pays off under which conditions* instead of
applying a fixed rule everywhere. That is the "recursive improvement" loop:
measure, update the defaults, measure again.

### Why "branch trimming" matters
The space of possible settings is enormous (shape × order × refinement
thresholds × …). The lab explores it cheaply first — coarse meshes, quick
solves — ranks the candidates, and only promotes the promising ones to
expensive high-fidelity runs (successive halving). You get the shape of the
cost/accuracy frontier without paying for the whole grid at full resolution.

---

## 5. How to follow the code

Start here and follow the includes:

- `src/fea/hierarchical.hpp` — the p-basis: 1-D Lobatto functions, per-entity
  modes on tet and hex, single-element stiffness. Read the header comment
  first; it states the nesting/minimum-rule contract.
- `src/fea/hp_assembly.hpp` — turns a mesh of hierarchical elements into one
  global system: per-entity DOF numbering, the minimum rule, the solve, and
  the energy-norm error used to check convergence.
- `tests/test_hierarchical.cpp`, `tests/test_hp_assembly.cpp` — the proofs.
  If you change the basis or the assembler, these tell you immediately whether
  conformity and convergence still hold.
- `src/fea/assembly.cpp` + `src/fea/vem.hpp` — mixed FE+VEM scatter into one K.
- `src/mesh/mixed_fill.hpp` (`native_poly_transitions`) and
  `VolumeMesher::kHybridVem` in `src/pipeline/scene.hpp` — unsplit transition
  cells as VEM PolyCells next to FE hex.
- `tests/test_fe_vem_assembly.cpp` — FE/VEM interface constant-strain gate.
- `docs/decisions/0019-mixed-fe-vem-adaptive-order-core.md` — the *why* behind
  every choice above, and the staging plan.
- `docs/decisions/0012-hybrid-graded-tet.md` — the meshers that produce the
  cells, including the curvature-driven h-refinement and the transition
  handling the VEM path keeps whole when native-poly is on.

The current status of each piece — what is built, what is next — is always in
`docs/dag/PROGRAM.yaml`.
