# Protecting balls + local feature size (LFS)

- Status: normative formula (ADR-0024 Q6); implementation node **M8**
- Normative: [ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md) §4,
  [ADR-0024](../decisions/0024-advisor-measure-answers.md) Q6
- Program: [advisor-measure-first-program.md](../plans/advisor-measure-first-program.md) §4.2
- Related: [varyhedron-packing.md](varyhedron-packing.md) §5,
  [geogram-cvt-vendoring.md](geogram-cvt-vendoring.md) (constrained sharp sites),
  [campaign-metrics.md](campaign-metrics.md)

Short agent note: how large protecting balls may be, where they apply, and which
cases break if LFS is ignored.

---

## 1. Role

Protecting balls (or protecting segments) keep **sharp CAD edge** samples as
constrained sites so the tet/CVT scaffold cannot Steiner-chop profiles (hole
rims, crease curves). They are **not** applied to smooth dihedrals or OCC
parameterization seams.

| Edge class | Action |
|------------|--------|
| **Sharp (G0)** | Protecting balls + hard snap |
| **Smooth / tangent-continuous** | No protect; free-slide wall + OCC project |
| **OCC seam** | Never protect |

During restricted CVT (Lane G), sharp protected sites stay **fixed constrained
sites**; only free sites move under Lloyd.

---

## 2. CDS radius formula (must-change)

\[
r = \min(\alpha\, h,\; \beta \cdot \mathrm{lfs}),
\quad \alpha \approx 0.45,\; \beta \approx 1/3
\]

| Symbol | Meaning |
|--------|---------|
| \(h\) | Local size field at the sample (same family as meshing / N_pred) |
| \(\mathrm{lfs}\) | Distance to nearest **other / non-adjacent** sharp geometry (sampled OK) |
| \(\alpha \approx 0.45\) | Fraction of local \(h\) — classical CDS-scale ball vs target edge length |
| \(\beta \approx 1/3\) | Cap so balls cannot swallow thin walls or neighboring sharp features |

**Corner shrink:** near CAD vertices / corners, shrink balls first (corner-ball
spirit) so concurrent edge balls do not over-cover the vertex neighborhood or
force impossible min-separation.

**Overlaps are intentional.** Overlapping balls with min site separation on the
order of \(\sim 0.55\, r\) (or an equivalent \(r\)-scaled rule) are expected for
continuous edge coverage — do not “fix” overlaps by globally shrinking until
edges go unprotected.

---

## 3. Reference

Primary theory/reference line:

- **Cheng, Dey, Shewchuk** — *Delaunay Mesh Generation* (CRC Press) — chapters
  on **protecting balls / segments**, PSC / PLC constraints, and how balls
  guarantee that constrained edges survive Delaunay refinement.

Related practice: CGAL Mesh_3 protecting-ball ideas; TetGen-style CDT recovery
papers (Si TOMS 2015) for *algorithms* only — **do not** vendor AGPL/GPL code
into core ([ADR-0023](../decisions/0023-measure-first-tet-primary-cvt-path.md) §6).

---

## 4. Risk cases

| Case | Failure mode without LFS clamp / corner shrink |
|------|--------------------------------------------------|
| **`plate_hole`** | Balls sized only on \(h\) can bridge hole rim ↔ outer/adjacent edges; hole profile lost or double-constrained; SCF neighborhood polluted |
| **`icecream_cone`** | Thin walls / converging sharp features: oversized balls swallow the wall thickness or adjacent creases → bad seeds, residual blowups, over_budget cascades |
| Dense crease clusters | Without \(\beta\cdot\mathrm{lfs}\), one edge’s balls dominate neighbors; chordal residual and Hausdorff become meaningless |

**Check when changing M8:** sharp edges remain protected; smooth walls and seams
are free; min_sep ~0.55·r-scale still allows intended overlap; plate_hole hole
rim and icecream thin features do not get balls larger than \(\beta\cdot\mathrm{lfs}\).

---

## 5. Agent checklist

- [ ] \(r = \min(\alpha h, \beta\cdot\mathrm{lfs})\) with \(\alpha\approx 0.45\), \(\beta\approx 1/3\)
- [ ] lfs from non-adjacent sharp geometry, not from the same edge sample
- [ ] Corner / vertex shrink applied
- [ ] Sharp-only; no OCC seams
- [ ] Overlaps intentional — do not treat overlap as a packing bug by itself
- [ ] Constrained sites stay fixed when Lloyd CVT lands (G2/G3)
