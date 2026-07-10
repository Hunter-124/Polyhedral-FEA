# ADR-0006: GUI — after the solver core, as phase P6.5

- Status: accepted (2026-07-09, GATE 0)
- Decision: scope addition (SPEC originally pinned "no GUI" for v1)

## Decision
A desktop GUI is in scope for v1 release, built after the adaptive loop works
(new phase P6.5, between performance engineering and OSS release):
- `gui` crate: wgpu 3D viewport + egui panels.
- Capabilities: geometry import, mesh preview with element-type/order
  coloring, load/BC setup, run control, stress and error-field visualization.
- Visual style: clean dark CAD-style theme to match the owner's desktop CAD
  application; theme tokens live in one module so it can be tuned against
  screenshots of that app when the phase starts.
- The CLI + VTU/ParaView path remains the automation and CI path; the GUI is
  a frontend over the same library APIs and adds no physics logic.

## Alternatives
- GUI-first or parallel-track: matures the app early but taxes every physics
  phase with UI upkeep.
- Never (original SPEC): rejected by owner — a polished GUI is part of what
  makes this OSS project compelling.

## Why
Owner decision: solver correctness first, then a GUI worth showing.
