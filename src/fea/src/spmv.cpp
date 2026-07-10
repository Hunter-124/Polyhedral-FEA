// SPDX-License-Identifier: BSD-3-Clause
#include "fea/spmv.hpp"

#include "fea/backend.hpp"
#include "fea/nodal_mesh.hpp"

#include <format>

namespace polymesh::fea {

#ifdef POLYMESH_WITH_CUDA
namespace cuda {
// backend_cuda.cu
bool device_available();
bool spmv_csr(std::size_t rows, std::size_t cols, const int* row_ptr, const int* col_idx,
              const double* values, const double* x, double* y);
} // namespace cuda
#endif

CsrMatrix csr_from_eigen(const Eigen::SparseMatrix<double>& a) {
    Eigen::SparseMatrix<double, Eigen::RowMajor> row = a;
    row.makeCompressed();

    CsrMatrix csr;
    csr.rows = static_cast<std::size_t>(row.rows());
    csr.cols = static_cast<std::size_t>(row.cols());
    const auto nnz = static_cast<std::size_t>(row.nonZeros());
    csr.row_ptr.resize(csr.rows + 1);
    csr.col_idx.resize(nnz);
    csr.values.resize(nnz);

    const auto* outer = row.outerIndexPtr();
    const auto* inner = row.innerIndexPtr();
    const auto* val = row.valuePtr();
    for (std::size_t i = 0; i <= csr.rows; ++i) {
        csr.row_ptr[i] = static_cast<int>(outer[i]);
    }
    for (std::size_t k = 0; k < nnz; ++k) {
        csr.col_idx[k] = static_cast<int>(inner[k]);
        csr.values[k] = val[k];
    }
    return csr;
}

void spmv_cpu(const CsrMatrix& a, std::span<const double> x, std::span<double> y) {
    if (x.size() != a.cols || y.size() != a.rows) {
        throw FeaError(std::format("spmv_cpu: size mismatch (A {}x{}, x {}, y {})", a.rows,
                                   a.cols, x.size(), y.size()));
    }
    if (a.row_ptr.size() != a.rows + 1) {
        throw FeaError("spmv_cpu: invalid CSR row_ptr length");
    }
    for (std::size_t row = 0; row < a.rows; ++row) {
        const int begin = a.row_ptr[row];
        const int end = a.row_ptr[row + 1];
        if (begin < 0 || end < begin || static_cast<std::size_t>(end) > a.values.size()) {
            throw FeaError(std::format("spmv_cpu: bad CSR range on row {}", row));
        }
        double sum = 0.0;
        for (int j = begin; j < end; ++j) {
            const int col = a.col_idx[static_cast<std::size_t>(j)];
            if (col < 0 || static_cast<std::size_t>(col) >= a.cols) {
                throw FeaError(std::format("spmv_cpu: column index {} out of range", col));
            }
            sum += a.values[static_cast<std::size_t>(j)] * x[static_cast<std::size_t>(col)];
        }
        y[row] = sum;
    }
}

bool try_spmv_cuda(const CsrMatrix& a, std::span<const double> x, std::span<double> y) {
#ifdef POLYMESH_WITH_CUDA
    if (!cuda::device_available()) {
        return false;
    }
    if (x.size() != a.cols || y.size() != a.rows) {
        throw FeaError(std::format("try_spmv_cuda: size mismatch (A {}x{}, x {}, y {})",
                                   a.rows, a.cols, x.size(), y.size()));
    }
    if (a.row_ptr.size() != a.rows + 1) {
        throw FeaError("try_spmv_cuda: invalid CSR row_ptr length");
    }
    return cuda::spmv_csr(a.rows, a.cols, a.row_ptr.data(), a.col_idx.data(), a.values.data(),
                          x.data(), y.data());
#else
    (void)a;
    (void)x;
    (void)y;
    return false;
#endif
}

void spmv(const CsrMatrix& a, std::span<const double> x, std::span<double> y) {
    if (active_backend() == Backend::kCuda) {
        if (try_spmv_cuda(a, x, y)) {
            return;
        }
    }
    spmv_cpu(a, x, y);
}

} // namespace polymesh::fea
