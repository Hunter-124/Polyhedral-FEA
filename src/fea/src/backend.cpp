// SPDX-License-Identifier: BSD-3-Clause
#include "fea/backend.hpp"

#include <Eigen/Core>

#include <format>
#include <mutex>

#if defined(POLYMESH_WITH_OPENMP)
#include <omp.h>
#endif

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

bool openmp_enabled() {
#if defined(POLYMESH_WITH_OPENMP)
    return true;
#else
    return false;
#endif
}

int openmp_max_threads() {
#if defined(POLYMESH_WITH_OPENMP)
    return omp_get_max_threads();
#else
    return 1;
#endif
}

std::string performance_description() {
    if (openmp_enabled()) {
        // Eigen stays single-threaded so it does not nest inside our OpenMP
        // element loops (nested OpenMP + LDLT can hang under oversubscription).
        return std::format("{} | OpenMP {} threads | Eigen serial (no nest)",
                           backend_description(), openmp_max_threads());
    }
    return std::format("{} | serial (OpenMP off)", backend_description());
}

void init_runtime_performance() {
    static std::once_flag once;
    std::call_once(once, [] {
#if defined(POLYMESH_WITH_OPENMP)
        // Parallelism is owned by element/mesh OpenMP loops (assembly, ZZ, SpMV,
        // grid classify). Keep Eigen at 1 thread so dense Ke / LDLT / small
        // solves never nest OpenMP — avoids deadlocks when ctest -j N runs many
        // processes each with OMP_NUM_THREADS=hw_cores.
        Eigen::setNbThreads(1);
        // OpenMP 3.0+ API; MSVC ships OpenMP 2.0 only (omp_set_nested).
#if defined(_OPENMP) && _OPENMP >= 200805
        omp_set_max_active_levels(1);
#else
        omp_set_nested(0);
#endif
#else
        Eigen::setNbThreads(1);
#endif
    });
}

} // namespace polymesh::fea
