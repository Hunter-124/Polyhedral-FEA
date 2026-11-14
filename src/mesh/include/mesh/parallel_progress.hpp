// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "mesh/fill_progress.hpp"

#include <atomic>
#include <climits>
#include <cstddef>
#include <exception>
#include <mutex>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace polymesh::mesh {

/// Run body(i) for every i in [0, n) on the OpenMP team. Calls must be
/// independent: each may read shared state but writes only its own slot.
///
/// The calling thread is team thread 0 and keeps reporting progress; workers
/// never reach the fill observer (it is thread-local, so their polls are
/// no-ops). The first exception from a body or from the observer stops the
/// remaining work and is rethrown here once every worker has finished.
template <class Body>
void parallel_for_progress(std::size_t n, Body&& body, std::size_t min_parallel = 256) {
#ifdef _OPENMP
    if (n >= min_parallel && n < static_cast<std::size_t>(INT_MAX) && omp_get_max_threads() > 1) {
        std::atomic<std::size_t> done{0};
        std::atomic<bool> failed{false};
        std::exception_ptr error;
        std::mutex error_mtx;
        const auto fail = [&] {
            std::lock_guard<std::mutex> lock(error_mtx);
            if (!error)
                error = std::current_exception();
            failed.store(true, std::memory_order_relaxed);
        };
        const int count = static_cast<int>(n);
#pragma omp parallel
        {
            const bool reporter = omp_get_thread_num() == 0;
#pragma omp for schedule(dynamic, 16)
            for (int i = 0; i < count; ++i) {
                if (failed.load(std::memory_order_relaxed))
                    continue;
                try {
                    body(static_cast<std::size_t>(i));
                } catch (...) {
                    fail();
                    continue;
                }
                const std::size_t finished = done.fetch_add(1, std::memory_order_relaxed) + 1;
                if (reporter) {
                    try {
                        fill_progress_poll(finished, n);
                    } catch (...) {
                        fail();
                    }
                }
            }
        }
        if (error)
            std::rethrow_exception(error);
        return;
    }
#else
    (void)min_parallel;
#endif
    for (std::size_t i = 0; i < n; ++i) {
        fill_progress_poll(i, n);
        body(i);
    }
}

} // namespace polymesh::mesh
