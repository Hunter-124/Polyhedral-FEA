# Archive

Frozen history as of commit `85f2b16`. Files here are kept for provenance and
are not maintained; edits are limited to archive banners and relative-link
fixes. They do not describe current status or policy: current status and next work are in
[docs/STATUS.md](../STATUS.md); binding decisions are the ADRs in
[docs/decisions/](../decisions/).

Varyhedron (`kVaryhedron`, variable-polyhedron packing,
[ADR-0021](../decisions/0021-varyhedron-packing.md)) is a core, active product
direction. Its research notes are archived because the survey phase is
finished, not because the direction was dropped.

| Path | What it was |
|------|-------------|
| [spec.md](spec.md) | Original v1 product spec and GATE 0 decision list (2026-07-09); superseded by README and the ADRs |
| [phases.md](phases.md) | P0–P6.5 phase plan with gates; all phases complete |
| [progress-history.md](progress-history.md) | Dated implementation chronology formerly in `docs/progress.md` |
| [ideas-from-fea-madness.md](ideas-from-fea-madness.md) | Triage of ideas from an external generated spec (2026-07-10) |
| [plans/mesher-solver-overhaul.md](plans/mesher-solver-overhaul.md) | Track H mesher honesty/performance overhaul plan (2026-07-10); led to ADR-0018 |
| [plans/variable-everything-and-advisor.md](plans/variable-everything-and-advisor.md) | Variable-everything meshing + learned advisor program plan (2026-08-09); led to ADR-0026/0027 |
| [research/varyhedron-packing.md](research/varyhedron-packing.md) | Varyhedron packing algorithm survey and ranking (ADR-0021, refined by ADR-0023) |
| [research/protecting-balls-lfs.md](research/protecting-balls-lfs.md) | Protecting-ball radius vs local feature size (ADR-0024 Q6) |
| [research/geogram-cvt-vendoring.md](research/geogram-cvt-vendoring.md) | Geogram restricted-CVT vendoring study and as-built integration (ADR-0025) |
| [research/campaign-metrics.md](research/campaign-metrics.md) | Campaign scoring/dashboard/gate metric definitions; the binding rules are ADR-0024 and [plan §3](../plans/advisor-measure-first-program.md) |
| [research/brep-face-tag-bc.md](research/brep-face-tag-bc.md) | Design stub for B-rep face-tag loads/probes (ADR-0024 Q7) |
| [research/ideabank/](research/ideabank/) | Raw research-subagent notes (2026-08-09) behind ADR-0026/0027: CAD corpora, ML prior art, label generation, inference deployment, mesh knobs, pre-solved features, wild ideas |
| [dag/PROGRAM.yaml](dag/PROGRAM.yaml) | Measure-first execution board; ran to completion (M0–M13, G0–G4) |
| [dag/README.md](dag/README.md) | How the board was claimed and worked (retired process) |
| [training/HANDOFF-3080ti.md](training/HANDOFF-3080ti.md) | v4 corpus regeneration run plan for the 3080 Ti training box (2026-08-13) |
