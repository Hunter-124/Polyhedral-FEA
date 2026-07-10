# ADR-0007: Implementation language — C++20 (switched from Rust)

- Status: accepted (2026-07-09, GATE 0 revision)

## Decision
The project is C++20 with CMake + Ninja. The Rust scaffold from earlier the
same day was ported before any physics existed. Toolchain guardrails replace
the borrow checker: `-Werror` with a wide warning set, clang-format
enforcement, RAII-only memory management, sanitizer CI planned, Catch2 tests.

## Alternatives
- Rust (original choice): stronger memory safety, but every heavyweight
  neighbor in this domain — OpenCASCADE, CUDA, CalculiX, VTK — is C/C++ and
  would live behind FFI.

## Why
Owner decision. Concrete wins: OpenCASCADE binds natively instead of through
maintained-by-one-person FFI crates; CUDA is first-class (ADR-0008); the FEA
reference ecosystem is directly linkable. The cost (manual memory discipline)
is mitigated by the guardrails above.
