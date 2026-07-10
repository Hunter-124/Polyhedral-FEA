// SPDX-License-Identifier: BSD-3-Clause
// CUDA device discovery for the backend dispatch layer (ADR-0008).
// Compute kernels (batched element stiffness, SpMV for iterative solves)
// land here as the phases that need them arrive; every kernel gets a CPU
// reference twin and a parity test in bench.

#include <cuda_runtime.h>

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

} // namespace polymesh::fea::cuda
