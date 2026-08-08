// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "fea/nodal_mesh.hpp"

#include <Eigen/Core>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace polymesh::fea {

/// Fraction of currently available system memory that a solve may consume.
/// Keeping 30% free leaves room for the OS, the GUI, CAD data, and allocator
/// fragmentation while a factorization is at its peak.
inline constexpr double kMemorySafetyFraction = 0.70;

/// Used only when the operating system cannot report available memory.  One
/// GiB is deliberately conservative: its 70% effective cap refuses large runs
/// instead of guessing that an unknown machine has ample RAM.
inline constexpr std::uint64_t kUnknownMemoryFallbackBytes = 1ULL << 30;

enum class MemoryAvailabilitySource {
    kProcMeminfo,
    kSysinfo,
    kConservativeDefault,
};

struct MemoryAvailability {
    std::uint64_t bytes = kUnknownMemoryFallbackBytes;
    MemoryAvailabilitySource source = MemoryAvailabilitySource::kConservativeDefault;
};

/// Peak-memory breakdown for one elastostatics solve.  CSR nonzeros are a
/// conservative upper bound computed from the mesh's actual element
/// connectivities: every element contributes its dense 3n x 3n local block;
/// duplicates shared by adjacent elements only make the bound safer.
struct SolveResourceEstimate {
    Eigen::Index ndof = 0;
    Eigen::Index nfree = 0;
    std::uint64_t csr_nnz_upper = 0;

    std::uint64_t node_storage_bytes = 0;
    std::uint64_t cell_storage_bytes = 0;
    std::uint64_t sparse_system_bytes = 0;
    std::uint64_t assembly_workspace_bytes = 0;
    std::uint64_t rhs_solution_bytes = 0;
    std::uint64_t ldlt_factor_bytes = 0;
    std::uint64_t cg_workspace_bytes = 0;

    std::uint64_t common_peak_bytes = 0;
    std::uint64_t direct_peak_bytes = 0;
    std::uint64_t cg_peak_bytes = 0;
};

struct EffectiveMemoryBudget {
    MemoryAvailability available;
    std::uint64_t safety_cap_bytes = 0;
    std::uint64_t user_cap_bytes = 0; // 0 = auto
    std::uint64_t effective_cap_bytes = 0;
};

/// Read memory currently available to new allocations.  Linux uses
/// /proc/meminfo's MemAvailable first and sysinfo(2) as a fallback.  Other
/// platforms, or unreadable Linux sources, use kUnknownMemoryFallbackBytes.
[[nodiscard]] MemoryAvailability system_memory_available();

/// Resolve max_mem_gb (decimal GB, 0 = auto) against 70% of MemAvailable.
[[nodiscard]] EffectiveMemoryBudget effective_memory_budget(double max_mem_gb);

/// Estimate the complete peak footprint before stiffness assembly allocates.
[[nodiscard]] SolveResourceEstimate estimate_solve_resources(const NodalMesh& mesh,
                                                              Eigen::Index nfree);

/// Largest named component for a method-specific estimate.
[[nodiscard]] std::string_view limiting_resource_term(const SolveResourceEstimate& estimate,
                                                      bool direct);

/// Human-readable IEC byte count (for example, "1.25 GiB").
[[nodiscard]] std::string format_memory_bytes(std::uint64_t bytes);

} // namespace polymesh::fea
