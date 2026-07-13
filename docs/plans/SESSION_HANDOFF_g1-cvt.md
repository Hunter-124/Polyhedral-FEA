# Session handoff — post G1–G4 / M5 + campaign-1

**Date:** 2026-07-13 (overnight arc)  
**Audience:** next agent  
**Canonical plan:** [advisor-measure-first-program.md](advisor-measure-first-program.md)  
**Board:** [PROGRAM.yaml](../dag/PROGRAM.yaml)

## Done this arc

| Node | Status | Notes |
|------|--------|--------|
| G1–G4 | **done** | Geogram CVT stack (prior) |
| M5 | **in_progress** | Domain clip + bnd snap; **cylinder health+load_area OK**; plate still FAIL; SE loses |
| campaign-1 | **done** | settings-frontier-1 finished 150 runs; SURVIVORS.md; defaults **not** flipped |
| feedback-loop | **todo** | Cycle-1 doc-only (hex suggestion rejected — ADR-0023) |

## M5 state (do not claim done)

- Campaign: `bench/campaigns/vem-gate-m5/` + [GATE.md](../../bench/campaigns/vem-gate-m5/GATE.md)
- **Cylinder:** `status=ok`, `health_ok=true`, `load_area_ok=true` (la_rel~0.048)
  - SE rel ~**0.41** vs hybrid ~**0.13** (lose accuracy)
  - First gate (AABB-only) had SE **0.087** but load_area fail
- **Plate:** load_area still ~2× expected; SCF ~0.54 vs hybrid 0.51
- Code path: `DomainClipParams` supporting halfspaces + convex-fail AABB fallback +
  boundary snap + soft wall inset (`scene.cpp` kCvtPoly)

### Next for M5

1. **Plate:** true RVD ∩ volume tet mesh (or explicit hole clip) — convex
   envelope halfspaces are not enough for load-face area
2. **Cylinder SE:** recover SE ≤ hybrid **without** losing load_area (0.65h
   densify improved SE slightly but broke la)
3. Re-run gate; promote only on both parts

## Open board

| id | status | notes |
|----|--------|--------|
| **M5** | in_progress | **start here** for critical path |
| V6d | todo | p>1 + curved edges |
| V11 | todo | packing iterate vs M9 after health_ok |
| feedback-loop | todo | next campaign on M9/M5 curved STEP |

## Order still locked

```
G* done → M5 VEM gate (headline) → V11 packing wins vs M9
```

## Verify

```bash
git pull --rebase
cmake --build build -j
./build/tests/polymesh_tests --reporter compact
./build/apps/testlab/polymesh_testlab run bench/campaigns/vem-gate-m5
```

Identity: Hunter-124, push master, no AI attribution.
