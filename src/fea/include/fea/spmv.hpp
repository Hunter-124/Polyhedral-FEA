// SPDX-License-Identifier: BSD-3-Clause
#pragma once

// CSR sparse matrix-vector product (SpMV) for free-DOF systems (CG path).
// Companion to Eigen CG in solve.hpp (F2); this API is the parity-tested
// reference for optional CUDA acceleration (F3 / ADR-0008).
//
// CPU reference always exists. CUDA implementation is compiled only when
// POLYMESH_WITH_CUDA is ON; try_spmv_cuda() returns false when the toolkit
// path is absent or no device is usable. Parity tests compare CPU SpMV to
// Eigen and (when available) CUDA SpMV to CPU.

#include <Eigen/SparseCore>

#include <cstddef>
#include <span>
#include <vector>

namespace polymesh::fea {

/// Compressed sparse row matrix (0-based column indices). Suitable for free
/// DOF blocks K_ff used by iterative solvers.
struct CsrMatrix {
    std::size_t rows = 0;
    std::size_t cols = 0;
    /// Length rows + 1; row i owns values/col_idx in [row_ptr[i], row_ptr[i+1]).
    std::vector<int> row_ptr;
    std::vector<int> col_idx;
    std::vector<double> values;

    std::size_t nnz() const { return values.size(); }
};

/// Build CSR from Eigen (any storage order). Zeros are dropped by Eigen compress.
CsrMatrix csr_from_eigen(const Eigen::SparseMatrix<double>& a);

/// y = A * x on the host. Sizes must match: x.size()==cols, y.size()==rows.
void spmv_cpu(const CsrMatrix& a, std::span<const double> x, std::span<double> y);

/// y = A * x using the active backend (CUDA when compiled in and a device is
/// present, otherwise CPU).
void spmv(const CsrMatrix& a, std::span<const double> x, std::span<double> y);

/// Device SpMV when POLYMESH_WITH_CUDA and a usable GPU exist. Returns false
/// (leaves y untouched) if CUDA is not available at compile or run time.
bool try_spmv_cuda(const CsrMatrix& a, std::span<const double> x, std::span<double> y);

} // namespace polymesh::fea
