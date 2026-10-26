// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>

namespace f3d::core {

struct Summary {
    std::size_t count = 0;
    double average = 0.0;
    double min = 0.0;
    double max = 0.0;
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
};

/// Summary statistics of `samples` (nearest-rank percentiles). All zero when empty.
[[nodiscard]] Summary summarize(std::span<const double> samples);

/// Nearest-rank percentile, `p` in [0, 100]. 0 when empty.
[[nodiscard]] double percentile(std::span<const double> samples, double p);

} // namespace f3d::core
