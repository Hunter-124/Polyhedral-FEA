// SPDX-License-Identifier: AGPL-3.0-or-later
#include "fea/backend.hpp"

namespace polymesh::fea {

#ifdef POLYMESH_WITH_CUDA
// Implemented in backend_cuda.cu.
namespace cuda {
bool device_available();
std::string device_name();
} // namespace cuda
#endif

Backend active_backend() {
#ifdef POLYMESH_WITH_CUDA
    if (cuda::device_available()) {
        return Backend::kCuda;
    }
#endif
    return Backend::kCpu;
}

std::string backend_description() {
#ifdef POLYMESH_WITH_CUDA
    if (cuda::device_available()) {
        return "cuda (" + cuda::device_name() + ")";
    }
#endif
    return "cpu";
}

} // namespace polymesh::fea
