#pragma once

#include "domain.h"
#include <numeric>

namespace ashare {

inline double averageClose(const std::vector<KLine>& lines, size_t count) {
    if (lines.size() < count || count == 0) return 0.0;
    return std::accumulate(lines.end() - static_cast<std::ptrdiff_t>(count), lines.end(), 0.0,
                           [](double sum, const KLine& line) { return sum + line.close; }) /
           static_cast<double>(count);
}

struct MacdValues {
    double dif = 0.0, dea = 0.0, histogram = 0.0, previousHistogram = 0.0;
};

// 保留原持仓建议的 EMA 初值和 2*(DIF-DEA) 柱值约定。
inline std::vector<MacdValues> calculateMacdHistory(const std::vector<KLine>& lines,
                                                  size_t fast = 12, size_t slow = 26,
                                                  size_t signal = 9) {
    std::vector<MacdValues> result;
    if (lines.empty() || !fast || !slow || !signal) return result;
    double emaFast = lines.front().close, emaSlow = emaFast, dea = 0.0, previous = 0.0;
    for (const auto& line : lines) {
        emaFast = emaFast * (fast - 1.0) / (fast + 1.0) + line.close * 2.0 / (fast + 1.0);
        emaSlow = emaSlow * (slow - 1.0) / (slow + 1.0) + line.close * 2.0 / (slow + 1.0);
        double dif = emaFast - emaSlow;
        dea = dea * (signal - 1.0) / (signal + 1.0) + dif * 2.0 / (signal + 1.0);
        double histogram = 2.0 * (dif - dea);
        result.push_back({dif, dea, histogram, previous});
        previous = histogram;
    }
    return result;
}

inline MacdValues calculateMacd(const std::vector<KLine>& lines) {
    auto history = calculateMacdHistory(lines);
    return history.empty() ? MacdValues{} : history.back();
}

}  // namespace ashare
