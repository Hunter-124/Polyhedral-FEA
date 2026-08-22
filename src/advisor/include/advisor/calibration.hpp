// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <string>

namespace polymesh::advisor {

struct HostCalibration {
    std::string host;
    double flops_per_s = 0.0;
    double bytes_per_s = 0.0;
    double ref_mesh_ms = 0.0;
    std::string generated_utc;
};

/// Stable local host label for campaign rows and calibration files.
[[nodiscard]] std::string local_host_name();

/// Current UTC time in the row/calibration ISO-8601 representation.
[[nodiscard]] std::string utc_timestamp();

/// Roofline conversion used only for reporting. Training targets remain the
/// hardware-portable FLOP and byte counts.
[[nodiscard]] double predicted_seconds(double flops, double bytes,
                                       const HostCalibration& calibration);

} // namespace polymesh::advisor
