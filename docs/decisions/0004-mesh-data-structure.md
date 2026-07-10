# ADR-0004: Mesh data structure — face-based owner/neighbour

- Status: accepted (2026-07-09, GATE 0)
- Decision: D4

## Decision
The mesh is a flat list of polygonal faces, each with an owner cell and an
optional neighbour cell (OpenFOAM-style), plus cells as face-index lists.
Face vertex loops are oriented outward from the owner.

## Alternatives
- Half-face/half-edge: richer ordered traversal (useful for sweeping), but far
  more code and invariants than assembly, error patches, or local remesh need.

## Why
Simplest structure that represents arbitrary polyhedra, gives direct adjacency
for assembly and ZZ patches, and supports local remesh by face-list splicing.
