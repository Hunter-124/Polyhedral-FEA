# ADR-0002: License — AGPL-3.0-or-later

- Status: accepted (2026-07-09, GATE 0)
- Decision: D2

## Decision
The project is licensed AGPL-3.0-or-later. All files carry
`// SPDX-License-Identifier: AGPL-3.0-or-later`. Dual commercial licensing
(selling license exceptions) remains open as a future option; that requires
copyright to stay consolidated, so external contributions will need a CLA or
DCO+assignment policy before we accept them (decide before first external PR).

## Alternatives
- PolyForm Noncommercial / BUSL / FSL: source-available, not OSI open source.
- MPL-2.0: weak copyleft, maximal adoption, no service-provider copyleft.

## Why
Owner wants a genuine OSS project with the strongest copyleft. AGPL closes the
SaaS loophole (a hosted meshing/FEA service must publish source) while
permitting all use. Dependency policy updates accordingly: MIT/Apache-2.0
preferred, LGPL acceptable, GPL-family acceptable if AGPL-compatible.
