# ADR-0006: GUI — after the solver core, as phase P6.5

- Status: accepted (2026-07-09, GATE 0); **revised same day** — owner pulled
  the GUI forward to run alongside solver development. Stack locked to
  GLFW + Dear ImGui (docking) + OpenGL, the same as the owner's AI-CAD app,
  with its dark teal palette mirrored token-for-token. Until the P2/P3
  conforming mesher exists, the GUI solves on a clearly-labeled draft voxel
  mesher (v0) so import -> pick faces -> constrain/load -> solve -> results
  works end-to-end today.
- **Restyle (2026-07-10):** owner pivoted chrome + controls to the Interwebz
  v2 look (deep plum panels, header-strip group boxes, rose gradient
  accents, lavender labels, fixed constrained window layout). The reference
  source (KingsleydotDev/ImGui-Menus) is an unlicensed leak, so nothing was
  copied: the look was re-implemented clean-room in `src/gui/widgets.cpp`
  on public Dear ImGui APIs, with only color values and layout proportions
  taken from visual inspection. The 3D viewport keeps the light studio
  canvas.
- Decision: scope addition (SPEC originally pinned "no GUI" for v1)

## Decision
A desktop GUI is in scope for v1 release, built after the adaptive loop works
(new phase P6.5, between performance engineering and OSS release):
- `gui` module: native C++ 3D viewport (OpenGL/Vulkan) + immediate-mode or Qt
  panels — toolkit finalized at phase start (owner's CAD app lives at
  ~/Repositories/CAD; match its stack and styling).
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
