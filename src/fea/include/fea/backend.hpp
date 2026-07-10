// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// Compute backend selection (ADR-0008).
//
// Parallelizable kernels (element stiffness batches, sparse matrix-vector
// products, error-indicator evaluation) dispatch through this layer. The CPU
// path is the reference implementation and always exists; the CUDA path is
// compiled in with POLYMESH_WITH_CUDA and selected at runtime only when a
// device is present. Both paths compute in double precision, and every CUDA
// kernel must match the CPU reference within solver tolerance (enforced by
// backend-parity tests in bench).

#include <string>

namespace polymesh::fea {

enum class Backend { kCpu, kCuda };

/// The backend kernels will dispatch to: kCuda when CUDA support is compiled
/// in and at least one device is usable, otherwise kCpu.
Backend active_backend();

/// Human-readable description, e.g. "cpu" or "cuda (NVIDIA GeForce RTX 3080 Ti)".
std::string backend_description();

} // namespace polymesh::fea
