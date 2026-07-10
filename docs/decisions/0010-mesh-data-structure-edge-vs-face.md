# ADR-0010: Mesh data structure — keep face-based; edge index optional

- Status: accepted (2026-07-10)
- Related: ADR-0004

## Context
Owner proposed an edge-primary store: length, orientation angle, incident
elements, endpoint connectivity — potentially easier for experimental hybrid
generators, possibly bulkier / less assembly-friendly.

## Decision
**Primary topology remains face-based owner/neighbour** (ADR-0004).

If generators need ordered edge walks, add a **derived edge adjacency index**
(built on demand from faces), not a second source of truth.

## Alternatives
| Option | Pros | Cons |
|---|---|---|
| Face owner/neighbour (current) | Arbitrary polyhedra, direct cell adjacency for assembly/ZZ, simple splice for local remesh | Edge loops reconstructed |
| Edge-primary (owner proposal) | Natural for feature edges, some generators | Duplicates polyhedron bounding info; assembly still wants faces; two truth sources if faces also stored |
| Full half-edge | Rich ordered traversal | Heavy invariants for little gain on VEM/tet fill |

## Why face-based wins for PolyMesh
1. **Assembly and ZZ patches** walk cell–face–cell adjacency.
2. **Hybrid zoo** (tet/hex/prism/pyramid/poly) is face-bounded by definition.
3. **Local remesh** splices face lists; edge-primary would re-derive faces.
4. Edge length/orientation are **attributes**, not topology — store on a
   derived edge table when feature grading needs them.

## Rejected for now
Making edges the sole owner of connectivity. Revisit only if a generator
benchmark proves face-based is the bottleneck (document measurements first).
