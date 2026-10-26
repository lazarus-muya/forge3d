// SPDX-License-Identifier: Apache-2.0
#include "f3d/core/stats.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace f3d::core {
namespace {

double nearest_rank(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(p, 0.0, 100.0);
    const auto rank = static_cast<std::size_t>(std::ceil(clamped / 100.0 * static_cast<double>(sorted.size())));
    return sorted[rank == 0 ? 0 : rank - 1];
}

} // namespace

double percentile(std::span<const double> samples, double p) {
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    return nearest_rank(sorted, p);
}

Summary summarize(std::span<const double> samples) {
    Summary s;
    if (samples.empty()) {
        return s;
    }
    std::vector<double> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    s.count = sorted.size();
    s.average = std::accumulate(sorted.begin(), sorted.end(), 0.0) / static_cast<double>(sorted.size());
    s.min = sorted.front();
    s.max = sorted.back();
    s.p50 = nearest_rank(sorted, 50.0);
    s.p95 = nearest_rank(sorted, 95.0);
    s.p99 = nearest_rank(sorted, 99.0);
    return s;
}

} // namespace f3d::core
