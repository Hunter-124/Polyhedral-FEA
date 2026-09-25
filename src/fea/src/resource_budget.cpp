// SPDX-License-Identifier: BSD-3-Clause
#include "fea/resource_budget.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(_WIN32)
// windows.h must precede psapi.h; the blank line keeps clang-format from sorting them.
#include <windows.h>

#include <psapi.h>
#endif

namespace polymesh::fea {
namespace {

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kGiB = 1ULL << 30;

std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a + b;
}

std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return a * b;
}

std::uint64_t index_as_u64(Eigen::Index value) {
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

std::uint64_t csr_bytes(std::uint64_t nnz, Eigen::Index rows) {
    // Eigen's default sparse storage is double values + int inner indices and
    // one int outer offset per column.
    return sat_add(sat_mul(nnz, sizeof(double) + sizeof(int)),
                   sat_mul(sat_add(index_as_u64(rows), 1), sizeof(int)));
}

std::uint64_t dense_square_cap(Eigen::Index rows) {
    const auto n = index_as_u64(rows);
    return sat_mul(n, n);
}

std::uint64_t ldlt_factor_nnz_envelope(std::uint64_t csr_nnz, Eigen::Index nfree) {
    if (nfree <= 0) {
        return 0;
    }

    // Early element-count planning has no reduced pattern to analyze, so use a
    // conservative 3-D envelope fitted to SimplicialLDLT fill (strict-L/CSR grew
    // from ~2.3 at 1.5k DOF to ~13 at 41k DOF); csr_nnz is already an upper
    // bound. Solve preflight passes the exact column count from analyze_solve_cost().
    const double scale = std::max(4.0, 1.8 * std::cbrt(static_cast<double>(nfree) / 1000.0));
    const long double predicted = static_cast<long double>(csr_nnz) * scale;
    const auto dense_lower = sat_mul(index_as_u64(nfree), sat_add(index_as_u64(nfree), 1)) / 2;
    if (predicted >= static_cast<long double>(dense_lower)) {
        return dense_lower;
    }
    return static_cast<std::uint64_t>(std::ceil(predicted));
}

} // namespace

MemoryAvailability system_memory_available() {
#if defined(__linux__)
    // MemAvailable accounts for reclaimable cache and is substantially more
    // useful than MemFree when deciding whether a new factorization will fit.
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::uint64_t value_kib = 0;
    std::string unit;
    while (meminfo >> key >> value_kib >> unit) {
        if (key == "MemAvailable:" && value_kib > 0) {
            return {.bytes = sat_mul(value_kib, kKiB),
                    .source = MemoryAvailabilitySource::kProcMeminfo};
        }
    }

    struct sysinfo info {};
    if (::sysinfo(&info) == 0 && info.mem_unit > 0) {
        const auto free_units = sat_add(static_cast<std::uint64_t>(info.freeram),
                                        static_cast<std::uint64_t>(info.bufferram));
        const auto bytes = sat_mul(free_units, static_cast<std::uint64_t>(info.mem_unit));
        if (bytes > 0) {
            return {.bytes = bytes, .source = MemoryAvailabilitySource::kSysinfo};
        }
    }
#elif defined(_WIN32)
    // ullAvailPhys is the Windows analogue of MemAvailable: physical memory
    // that can be allocated without paging.
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (::GlobalMemoryStatusEx(&status) != 0 && status.ullAvailPhys > 0) {
        return {.bytes = static_cast<std::uint64_t>(status.ullAvailPhys),
                .source = MemoryAvailabilitySource::kGlobalMemoryStatus};
    }
#endif
    return {.bytes = kUnknownMemoryFallbackBytes,
            .source = MemoryAvailabilitySource::kConservativeDefault};
}

std::uint64_t peak_resident_bytes() {
#if defined(__linux__)
    // VmHWM is the kernel's own high-water mark for this process, so it
    // survives the free() that follows a factorization and needs no sampling
    // loop to catch the peak. /proc/self/status mixes textual and numeric
    // fields, so it is read a line at a time rather than word by word.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        constexpr std::string_view kKey = "VmHWM:";
        if (line.compare(0, kKey.size(), kKey) != 0) {
            continue;
        }
        std::istringstream fields(line.substr(kKey.size()));
        std::uint64_t value_kib = 0;
        if (fields >> value_kib) {
            return sat_mul(value_kib, kKiB);
        }
        return 0;
    }
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) != 0) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#endif
    return 0;
}

EffectiveMemoryBudget effective_memory_budget(double max_mem_gb) {
    EffectiveMemoryBudget out;
    out.available = system_memory_available();
    out.safety_cap_bytes = static_cast<std::uint64_t>(
        std::floor(static_cast<long double>(out.available.bytes) * kMemorySafetyFraction));
    if (std::isfinite(max_mem_gb) && max_mem_gb > 0.0) {
        const long double requested = static_cast<long double>(max_mem_gb) * 1'000'000'000.0L;
        out.user_cap_bytes =
            requested >= static_cast<long double>(std::numeric_limits<std::uint64_t>::max())
                ? std::numeric_limits<std::uint64_t>::max()
                : static_cast<std::uint64_t>(std::floor(requested + 0.5L));
    }
    out.effective_cap_bytes = out.user_cap_bytes > 0
                                  ? std::min(out.user_cap_bytes, out.safety_cap_bytes)
                                  : out.safety_cap_bytes;
    return out;
}

SolveResourceEstimate estimate_solve_resources(const NodalMesh& mesh, Eigen::Index nfree,
                                               std::optional<std::uint64_t> exact_factor_nnz) {
    SolveResourceEstimate out;
    out.ndof = 3 * static_cast<Eigen::Index>(mesh.nodes.size());
    out.nfree = std::clamp(nfree, Eigen::Index{0}, out.ndof);

    out.node_storage_bytes =
        sat_mul(static_cast<std::uint64_t>(mesh.nodes.capacity()), sizeof(Eigen::Vector3d));
    out.cell_storage_bytes =
        sat_mul(static_cast<std::uint64_t>(mesh.elements.capacity()), sizeof(NodalElement));

    std::uint64_t connectivity_nnz = 0;
    for (const auto& element : mesh.elements) {
        const auto nnode = static_cast<std::uint64_t>(element.nodes.size());
        out.cell_storage_bytes =
            sat_add(out.cell_storage_bytes,
                    sat_mul(static_cast<std::uint64_t>(element.nodes.capacity()),
                            sizeof(std::uint32_t)));
        const auto local_dof = sat_mul(3, nnode);
        connectivity_nnz = sat_add(connectivity_nnz, sat_mul(local_dof, local_dof));
        out.cell_storage_bytes =
            sat_add(out.cell_storage_bytes,
                    sat_mul(static_cast<std::uint64_t>(element.faces.capacity()),
                            sizeof(std::vector<std::uint32_t>)));
        for (const auto& face : element.faces) {
            out.cell_storage_bytes = sat_add(
                out.cell_storage_bytes,
                sat_mul(static_cast<std::uint64_t>(face.capacity()), sizeof(std::uint32_t)));
        }
    }

    out.csr_nnz_upper = std::min(connectivity_nnz, dense_square_cap(out.ndof));
    const auto free_nnz = std::min(out.csr_nnz_upper, dense_square_cap(out.nfree));
    const auto global_csr = csr_bytes(out.csr_nnz_upper, out.ndof);
    const auto reduced_csr = csr_bytes(free_nnz, out.nfree);
    out.sparse_system_bytes = sat_add(global_csr, reduced_csr);

    // Peak during assembly + reduction: the assembled global CSR, the
    // connectivity pattern scratch that builds it, the bounded element-matrix
    // chunk, and the compressed K_ff, all live at once. The pattern scratch is
    // one uint32 per element node pair (a ninth of the DOF-level nonzero
    // bound) plus two node-length offset arrays.
    const auto node_pairs = out.csr_nnz_upper / 9;
    const auto n_nodes_u = index_as_u64(out.ndof) / 3;
    const auto pattern_scratch =
        sat_add(sat_mul(node_pairs, sizeof(std::uint32_t)),
                sat_mul(sat_add(n_nodes_u, 1), 2 * sizeof(std::uint64_t)));
    // `assemble_stiffness` sizes its element-matrix scratch to this budget.
    constexpr std::uint64_t kAssemblyChunkBytes = 32ULL << 20;
    out.assembly_workspace_bytes = sat_add(sat_add(global_csr, pattern_scratch),
                                           sat_add(kAssemblyChunkBytes, reduced_csr));

    const auto ndof_u = index_as_u64(out.ndof);
    const auto nfree_u = index_as_u64(out.nfree);
    // Caller-owned loads + full result + reduced map, rhs, and reduced result.
    out.rhs_solution_bytes =
        sat_add(sat_mul(ndof_u, 2 * sizeof(double) + sizeof(Eigen::Index)),
                sat_mul(nfree_u, 2 * sizeof(double)));

    const auto factor_nnz =
        exact_factor_nnz.value_or(ldlt_factor_nnz_envelope(free_nnz, out.nfree));
    out.ldlt_factor_nnz = factor_nnz;
    out.ldlt_factor_bytes = sat_add(csr_bytes(factor_nnz, out.nfree),
                                    sat_mul(nfree_u, sizeof(double) + 5 * sizeof(int)));

    // IncompleteCholesky keeps approximately one triangular copy of the input;
    // uninterrupted PCG adds x, r, z, p, Ap, and one temporary solve vector.
    const auto ichol_nnz = sat_add(free_nnz / 2, nfree_u);
    out.cg_workspace_bytes =
        sat_add(csr_bytes(ichol_nnz, out.nfree), sat_mul(nfree_u, 6 * sizeof(double)));

    out.common_peak_bytes =
        sat_add(sat_add(out.node_storage_bytes, out.cell_storage_bytes),
                sat_add(out.assembly_workspace_bytes, out.rhs_solution_bytes));
    out.direct_peak_bytes = sat_add(out.common_peak_bytes, out.ldlt_factor_bytes);
    out.cg_peak_bytes = sat_add(out.common_peak_bytes, out.cg_workspace_bytes);
    return out;
}

std::string_view limiting_resource_term(const SolveResourceEstimate& estimate, bool direct) {
    std::string_view name = "assembly workspace";
    std::uint64_t largest = estimate.assembly_workspace_bytes;
    const auto consider = [&](std::uint64_t bytes, std::string_view candidate,
                              std::uint64_t& current, std::string_view& current_name) {
        if (bytes > current) {
            current = bytes;
            current_name = candidate;
        }
    };
    consider(sat_add(estimate.node_storage_bytes, estimate.cell_storage_bytes), "mesh storage",
             largest, name);
    consider(estimate.rhs_solution_bytes, "RHS/solution vectors", largest, name);
    if (direct) {
        consider(estimate.ldlt_factor_bytes, "LDLT factor", largest, name);
    } else {
        consider(estimate.cg_workspace_bytes, "CG preconditioner/work vectors", largest, name);
    }
    return name;
}

std::string format_memory_bytes(std::uint64_t bytes) {
    if (bytes >= kGiB) {
        return std::format("{:.2f} GiB",
                           static_cast<double>(bytes) / static_cast<double>(kGiB));
    }
    if (bytes >= (1ULL << 20)) {
        return std::format("{:.1f} MiB",
                           static_cast<double>(bytes) / static_cast<double>(1ULL << 20));
    }
    if (bytes >= kKiB) {
        return std::format("{:.1f} KiB",
                           static_cast<double>(bytes) / static_cast<double>(kKiB));
    }
    return std::format("{} B", bytes);
}

} // namespace polymesh::fea
