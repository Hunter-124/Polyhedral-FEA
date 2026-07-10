# ADR-0007: Implementation language — C++20

- Status: accepted (2026-07-09, GATE 0; scrubbed 2026-07-10)

## Decision
The project is **C++20 only**, built with CMake + Ninja. Toolchain guardrails:
`-Werror` with a wide warning set, clang-format, RAII-only memory management,
sanitizer CI planned, Catch2 tests.

## Alternatives
- Other systems languages: rejected; the domain neighbors (OpenCASCADE, CUDA,
  CalculiX, VTK/VTU tooling) are native C/C++.

## Why
OpenCASCADE binds natively; CUDA is first-class (ADR-0008); FEA reference
ecosystem is directly linkable. Memory discipline is enforced by the
guardrails above, not by a second language.
