// SPDX-License-Identifier: BSD-3-Clause
// CUDA device discovery and SpMV kernel (ADR-0008 / ROADMAP F3).
// CPU reference twin: fea/spmv.hpp spmv_cpu. Parity tests in test_spmv.cpp.
// Batched element stiffness kernels land here as later F-track work needs them.

#include <cuda_runtime.h>

#include <cstddef>
#include <string>

namespace polymesh::fea::cuda {

bool device_available() {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

std::string device_name() {
    int device = 0;
    if (cudaGetDevice(&device) != cudaSuccess) {
        return "unknown";
    }
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, device) != cudaSuccess) {
        return "unknown";
    }
    return prop.name;
}

namespace {

__global__ void csr_spmv_kernel(std::size_t rows, const int* row_ptr, const int* col_idx,
                                const double* values, const double* x, double* y) {
    const std::size_t row =
        static_cast<std::size_t>(blockIdx.x) * static_cast<std::size_t>(blockDim.x) +
        static_cast<std::size_t>(threadIdx.x);
    if (row >= rows) {
        return;
    }
    const int begin = row_ptr[row];
    const int end = row_ptr[row + 1];
    double sum = 0.0;
    for (int j = begin; j < end; ++j) {
        sum += values[j] * x[col_idx[j]];
    }
    y[row] = sum;
}

template <typename T> void cuda_free(T*& p) {
    if (p != nullptr) {
        cudaFree(p);
        p = nullptr;
    }
}

} // namespace

bool spmv_csr(std::size_t rows, std::size_t cols, const int* row_ptr, const int* col_idx,
              const double* values, const double* x, double* y) {
    if (rows == 0) {
        return true;
    }
    if (!device_available() || row_ptr == nullptr || x == nullptr || y == nullptr) {
        return false;
    }

    const int nnz = row_ptr[rows];
    if (nnz < 0) {
        return false;
    }

    int* d_row_ptr = nullptr;
    int* d_col_idx = nullptr;
    double* d_values = nullptr;
    double* d_x = nullptr;
    double* d_y = nullptr;

    auto fail = [&]() {
        cuda_free(d_row_ptr);
        cuda_free(d_col_idx);
        cuda_free(d_values);
        cuda_free(d_x);
        cuda_free(d_y);
        return false;
    };

    if (cudaMalloc(&d_row_ptr, (rows + 1) * sizeof(int)) != cudaSuccess) {
        return fail();
    }
    if (nnz > 0) {
        if (cudaMalloc(&d_col_idx, static_cast<std::size_t>(nnz) * sizeof(int)) !=
            cudaSuccess) {
            return fail();
        }
        if (cudaMalloc(&d_values, static_cast<std::size_t>(nnz) * sizeof(double)) !=
            cudaSuccess) {
            return fail();
        }
    }
    if (cols > 0) {
        if (cudaMalloc(&d_x, cols * sizeof(double)) != cudaSuccess) {
            return fail();
        }
    }
    if (cudaMalloc(&d_y, rows * sizeof(double)) != cudaSuccess) {
        return fail();
    }

    if (cudaMemcpy(d_row_ptr, row_ptr, (rows + 1) * sizeof(int), cudaMemcpyHostToDevice) !=
        cudaSuccess) {
        return fail();
    }
    if (nnz > 0) {
        if (cudaMemcpy(d_col_idx, col_idx, static_cast<std::size_t>(nnz) * sizeof(int),
                       cudaMemcpyHostToDevice) != cudaSuccess) {
            return fail();
        }
        if (cudaMemcpy(d_values, values, static_cast<std::size_t>(nnz) * sizeof(double),
                       cudaMemcpyHostToDevice) != cudaSuccess) {
            return fail();
        }
    }
    if (cols > 0) {
        if (cudaMemcpy(d_x, x, cols * sizeof(double), cudaMemcpyHostToDevice) != cudaSuccess) {
            return fail();
        }
    }

    constexpr int kBlock = 256;
    const int blocks = static_cast<int>((rows + static_cast<std::size_t>(kBlock) - 1) /
                                        static_cast<std::size_t>(kBlock));
    csr_spmv_kernel<<<blocks, kBlock>>>(rows, d_row_ptr, d_col_idx, d_values, d_x, d_y);
    if (cudaGetLastError() != cudaSuccess) {
        return fail();
    }
    if (cudaDeviceSynchronize() != cudaSuccess) {
        return fail();
    }
    if (cudaMemcpy(y, d_y, rows * sizeof(double), cudaMemcpyDeviceToHost) != cudaSuccess) {
        return fail();
    }

    cuda_free(d_row_ptr);
    cuda_free(d_col_idx);
    cuda_free(d_values);
    cuda_free(d_x);
    cuda_free(d_y);
    return true;
}

} // namespace polymesh::fea::cuda
