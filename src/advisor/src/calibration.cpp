// SPDX-License-Identifier: BSD-3-Clause
#include "advisor/calibration.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <limits>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace polymesh::advisor {

std::string local_host_name() {
    std::array<char, 256> name{};
#if defined(_WIN32)
    DWORD size = static_cast<DWORD>(name.size());
    if (::GetComputerNameA(name.data(), &size) != 0 && size > 0) {
        return std::string(name.data(), size);
    }
#else
    if (::gethostname(name.data(), name.size()) == 0) {
        name.back() = '\0';
        if (name.front() != '\0') {
            return name.data();
        }
    }
#endif
    return "unknown-host";
}

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text.data();
}

double predicted_seconds(double flops, double bytes, const HostCalibration& calibration) {
    if (!std::isfinite(flops) || flops < 0.0 || !std::isfinite(bytes) || bytes < 0.0 ||
        !std::isfinite(calibration.flops_per_s) || calibration.flops_per_s <= 0.0 ||
        !std::isfinite(calibration.bytes_per_s) || calibration.bytes_per_s <= 0.0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::max(flops / calibration.flops_per_s, bytes / calibration.bytes_per_s);
}

} // namespace polymesh::advisor
