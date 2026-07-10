# ADR-0005: Benchmark baseline — own uniform tet10 path + CalculiX audit cross-check

- Status: accepted (2026-07-09, GATE 0)
- Decision: D5

## Decision
The headline claims (≥5× DOF reduction, ≥3× wall time at equal energy-norm
error) are measured against a uniform 2nd-order tet (tet10) path in our own
solver — same assembly, same linear solver, same error norm — frozen at
GATE 1. `/audit` additionally cross-checks Tier-1 cases against CalculiX to
catch bugs shared by both of our paths.

## Alternatives
- CalculiX as the primary baseline: independent, but ratios would mix mesh
  strategy with implementation differences — noise for the claim being made.

## Why
Self-consistency isolates the variable under test (the adaptive mesh
strategy). External cross-check preserves honesty without contaminating the
metric.
