// SPDX-License-Identifier: BSD-3-Clause

// `polymesh calibrate` (host FLOP/byte/reference-mesh rates) and
// `polymesh backend`.

#include "cli_common.hpp"

#include "advisor/calibration.hpp"
#include "fea/backend.hpp"
#include "pipeline/scene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace polymesh::cli {
namespace {

double median_sample(std::vector<double> samples) {
    if (samples.empty()) {
        throw std::runtime_error("calibrate: no benchmark samples");
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

double benchmark_flops_per_second() {
    using clock = std::chrono::steady_clock;
    constexpr std::size_t kIterations = 8'000'000;
    constexpr double kFlopsPerIteration = 16.0; // eight double FMAs
    std::vector<double> rates;
    rates.reserve(3);
    for (int trial = 0; trial < 3; ++trial) {
        std::array<double, 8> accumulators{
            0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8,
        };
        const auto started = clock::now();
        for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
            accumulators[0] = std::fma(1.0000001, 0.9999999, accumulators[0]);
            accumulators[1] = std::fma(1.0000002, 0.9999998, accumulators[1]);
            accumulators[2] = std::fma(1.0000003, 0.9999997, accumulators[2]);
            accumulators[3] = std::fma(1.0000004, 0.9999996, accumulators[3]);
            accumulators[4] = std::fma(1.0000005, 0.9999995, accumulators[4]);
            accumulators[5] = std::fma(1.0000006, 0.9999994, accumulators[5]);
            accumulators[6] = std::fma(1.0000007, 0.9999993, accumulators[6]);
            accumulators[7] = std::fma(1.0000008, 0.9999992, accumulators[7]);
        }
        const double seconds = std::chrono::duration<double>(clock::now() - started).count();
        volatile double sink = 0.0;
        for (const double value : accumulators) {
            sink = sink + value;
        }
        static_cast<void>(sink);
        if (!(seconds > 0.0)) {
            throw std::runtime_error("calibrate: FLOP timer did not advance");
        }
        rates.push_back(kFlopsPerIteration * static_cast<double>(kIterations) / seconds);
    }
    return median_sample(std::move(rates));
}

double benchmark_bytes_per_second() {
    using clock = std::chrono::steady_clock;
    constexpr std::size_t kValues = 1U << 23;               // three 64 MiB arrays
    constexpr double kBytesPerValue = 3.0 * sizeof(double); // two reads, one write
    std::vector<double> a(kValues, 0.0);
    std::vector<double> b(kValues, 1.0);
    std::vector<double> c(kValues, 2.0);
    std::vector<double> rates;
    rates.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        const auto started = clock::now();
        for (std::size_t i = 0; i < kValues; ++i) {
            a[i] = b[i] + 1.25 * c[i];
        }
        const double seconds = std::chrono::duration<double>(clock::now() - started).count();
        volatile double sink = a[static_cast<std::size_t>(trial)];
        static_cast<void>(sink);
        if (!(seconds > 0.0)) {
            throw std::runtime_error("calibrate: byte timer did not advance");
        }
        rates.push_back(kBytesPerValue * static_cast<double>(kValues) / seconds);
    }
    return median_sample(std::move(rates));
}

std::filesystem::path default_calibration_part(std::string_view argv0) {
    std::error_code error;
    std::filesystem::path executable;
#if defined(__linux__)
    executable = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
    if (executable.empty()) {
        error.clear();
        executable = std::filesystem::weakly_canonical(std::filesystem::path(argv0), error);
    }
    if (executable.empty()) {
        executable = std::filesystem::path(argv0);
    }
    const auto candidate = executable.parent_path().parent_path() / "share" / "polymesh" /
                           "calibration" / "plate_hole.step";
    if (!std::filesystem::is_regular_file(candidate, error)) {
        throw std::runtime_error(
            "calibrate: reference mesh asset is not installed; pass --reference part.step");
    }
    return candidate;
}

double benchmark_reference_mesh_ms(const std::filesystem::path& reference_part,
                                   double reference_h) {
    using clock = std::chrono::steady_clock;
    const auto model = polymesh::pipeline::Model::load(reference_part.string());
    std::vector<double> samples;
    samples.reserve(5);
    for (int trial = 0; trial < 5; ++trial) {
        const auto started = clock::now();
        const auto plan =
            polymesh::pipeline::build_refinement_plan(model, reference_h, {}, true, true, 0);
        auto volume = polymesh::pipeline::volume_mesh(
            model, reference_h, polymesh::pipeline::VolumeMesher::kGradedTet, 2, true,
            plan.refine_seeds, plan.seed_band, 0.0, 0, 0, 0, {}, plan.size_field);
        volume.mesh.check_validity();
        samples.push_back(
            std::chrono::duration<double, std::milli>(clock::now() - started).count());
    }
    return median_sample(std::move(samples));
}

} // namespace

int cmd_calibrate(std::span<char*> args) {
    std::filesystem::path output_path;
    std::filesystem::path reference_part;
    double reference_h = 0.004;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (std::strcmp(args[i], "--out") == 0 && i + 1 < args.size()) {
            output_path = args[++i];
        } else if (std::strcmp(args[i], "--reference") == 0 && i + 1 < args.size()) {
            reference_part = args[++i];
        } else if (std::strcmp(args[i], "--reference-h") == 0 && i + 1 < args.size()) {
            char* end = nullptr;
            reference_h = std::strtod(args[++i], &end);
            if (end == args[i] || *end != '\0' || !std::isfinite(reference_h) ||
                reference_h <= 0.0) {
                std::fputs("calibrate: --reference-h wants a positive mesh size in metres\n",
                           stderr);
                return usage();
            }
        } else {
            return usage();
        }
    }
    if (output_path.empty()) {
        std::fputs("calibrate: --out host.json is required\n", stderr);
        return usage();
    }
    if (reference_part.empty()) {
        reference_part = default_calibration_part(args[0]);
    }
    if (!std::filesystem::is_regular_file(reference_part)) {
        throw std::runtime_error("calibrate: reference part not found: " +
                                 reference_part.string());
    }

    polymesh::advisor::HostCalibration calibration;
    calibration.host = polymesh::advisor::local_host_name();
    calibration.flops_per_s = benchmark_flops_per_second();
    calibration.bytes_per_s = benchmark_bytes_per_second();
    calibration.ref_mesh_ms = benchmark_reference_mesh_ms(reference_part, reference_h);
    calibration.generated_utc = polymesh::advisor::utc_timestamp();
    if (!(calibration.flops_per_s > 0.0) || !(calibration.bytes_per_s > 0.0) ||
        !(calibration.ref_mesh_ms > 0.0)) {
        throw std::runtime_error("calibrate: benchmark produced a non-positive result");
    }

    const nlohmann::json document{
        {"host", calibration.host},
        {"flops_per_s", calibration.flops_per_s},
        {"bytes_per_s", calibration.bytes_per_s},
        {"ref_mesh_ms", calibration.ref_mesh_ms},
        {"generated_utc", calibration.generated_utc},
    };
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    std::ofstream output(output_path);
    if (!output) {
        throw std::runtime_error("calibrate: cannot write " + output_path.string());
    }
    output << document.dump(2) << '\n';
    if (!output) {
        throw std::runtime_error("calibrate: write failed for " + output_path.string());
    }
    std::printf("wrote %s\n", output_path.string().c_str());
    return 0;
}

int cmd_backend() {
    polymesh::fea::init_runtime_performance();
    std::printf("%s\n", polymesh::fea::performance_description().c_str());
    return 0;
}

} // namespace polymesh::cli
