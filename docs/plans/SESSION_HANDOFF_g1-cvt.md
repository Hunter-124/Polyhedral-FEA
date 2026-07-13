# Session handoff — M5 accuracy push (PC restart checkpoint)

**Date:** 2026-07-13  
**Audience:** next agent  
**Canonical plan:** [advisor-measure-first-program.md](advisor-measure-first-program.md)  
**Board:** [PROGRAM.yaml](../dag/PROGRAM.yaml)

## Done this arc

| Node | Status | Notes |
|------|--------|--------|
| G1–G4 | **done** | Geogram CVT stack |
| M5 health | **done** | RVD + load trim: both plate+cylinder `health_ok` + `load_area_ok` |
| M5 accuracy | **in_progress** | Closing on hybrid; **not** promoted |
| campaign-1 | **done** | settings-frontier-1; defaults not flipped |

## M5 state (do not claim done)

Campaign: `bench/campaigns/vem-gate-m5/` + [GATE.md](../../bench/campaigns/vem-gate-m5/GATE.md)

| part | mesher | primary rel_err | health |
|------|--------|-----------------|--------|
| plate_hole | hybrid_zoo | SCF **0.512** | ok |
| cylinder | hybrid_zoo | SE **0.132** | ok |
| plate_hole | cvt_poly | SCF **0.545** | **ok** |
| cylinder | cvt_poly | SE **0.135** | **ok** |

Gaps: plate ~0.033 SCF; cylinder ~0.0035 SE (almost there).

### Code levers currently in tree (`scene.cpp` kCvtPoly + `vem.cpp`)

- Free sites 0.9h + mild plate in-plane densify near sharp
- **Plate-only** soft wall inset; cylinder skips wall pull
- Cylinder **inset shell** free sites on curved wall
- RVD tet-clip for all solids; OCC bnd snap preferred
- VEM k=1 **τ = 0.08** (was 1.0) — SE/compliance win

### Failed / regressing (do not retry blindly)

- Polar rings around hole, graded Lloyd size, aggressive densify → **worse SCF**
- Global free ≤0.88h, wall densify → residual risk
- Cylinder AABB-only → load_area fail (SE looked good for wrong reasons)

### Next for M5

1. Close cylinder SE residual (~0.003) carefully
2. Plate SCF needs non-densify idea (face-mean dilution vs p99; hole-wall
   fidelity; recovery) — packing densify stuck ~0.545
3. Promote only when **both** beat hybrid with health_ok

## Open board

| id | status | notes |
|----|--------|--------|
| **M5** | in_progress | **start here** |
| V6d | todo | p>1 + curved edges |
| V11 | todo | packing iterate vs M9 after health_ok |
| feedback-loop | todo | next campaign on M9/M5 curved STEP |

## Verify

```bash
git pull --rebase
cmake --build build -j
./build/tests/polymesh_tests "[cvt]" --reporter compact
./build/apps/testlab/polymesh_testlab run bench/campaigns/vem-gate-m5
```

Identity: Hunter-124, push master, no AI attribution.
