# BRep face-tag BCs / probes (design stub)

- Status: design-only — **not** scheduled until after simple planar cases
  finish the `expected_area` box path (M12) and before curved loads / geometry
  sweeps (ADR-0024 **Q7**, plan §3.5).
- Normative: [ADR-0024](../decisions/0024-advisor-measure-answers.md) Q7,
  [advisor-measure-first-program.md](../plans/advisor-measure-first-program.md) §3.5,
  [interfaces.md](../dag/interfaces.md) load `select` schema.
- Related: [campaign-metrics.md](campaign-metrics.md) load-area gate.

## Why boxes are temporary

Current cases select Dirichlet / traction faces with **axis-aligned boxes** over
mesh-face (or node) centroids. That is robust on planar, axis-aligned CAD faces
when the box has slack and is h-independent.

**Guard today:** optional `loads[].select.expected_area` (m²). Harness compares
selected free-face area to the intended CAD face; relative error **> 2%** →
`health.load_area_ok = false`, `status = solve_suspect`.

Boxes **fail** when:

| Failure mode | Example |
|--------------|---------|
| Curved / non-planar load region | Sphere polar cap (mesh faces, not one CAD face) |
| Multi-face or boolean topology | Icecream scoop ∪ pyramid lateral faces |
| Geometry sweeps / parametric family | Face index or bounding box drifts with parameter |
| BC-adjacent stress metrics | Need the true loaded CAD face, not “whatever fell in the box” |

Q7: keep boxes for ~current planar fixtures **if** `expected_area` is set;
invest **~2–3 days** in face tags before curved loads, sweeps, or BC-adjacent
stress scores.

## Target model (sketch)

1. **Stable face IDs from CAD** — Prefer OCC-stable keys where available
   (e.g. shape history / named faces if STEP carries them; otherwise a
   deterministic index over `TopExp` faces of the loaded solid, frozen per
   fixture generation script). Document the key scheme next to
   `scripts/gen_cad_parts.py`.
2. **Case schema** — Extend select, e.g.:
   ```jsonc
   "select": {
     "face_ids": [3],           // or "face_names": ["x_max"]
     "expected_area": 0.001     // keep ±2% guard as regression
   }
   ```
   Boxes remain valid for legacy / planar cases; face_ids take precedence when set.
3. **Mesh association** — After surface mesh / free-face extract, map each mesh
   triangle (or boundary facet) to a BRep face via projection / topology from
   the same path that already samples OCC for curvature and edge classify.
   Aggregate area and DOFs by tag, not by AABB.
4. **Probes** — Hole-neighborhood SCF and tip face-mean already want a tagged
   region; unify load + probe selection on the same face-tag map.
5. **Anti-cheat** — Analytic `expected_area` stays in case JSON / hand-calcs only;
   never hard-code product face areas in `src/`.

## Out of scope for this stub

- Implementing face maps in geom/mesh or testlab
- Changing sphere / icecream BCs
- Virtual topology / defeaturing (M11 is flag-only)

## Exit criteria (future work item)

- Sphere and icecream (or any curved/multi-face load) can set `expected_area`
  against the **tagged** CAD face set and pass ±2% on campaign meshes
- Parameter sweep of plate dimensions does not require hand-editing boxes
- Docs: interfaces schema + one Catch2 or testlab self-check that a wrong face
  tag fails the area guard loudly
