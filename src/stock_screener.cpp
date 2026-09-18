#include "stock_screener.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace ashare {
namespace {

double validAmplitude(const KLine& line, double fallbackClose) {
    double reference = fallbackClose > 0.0 ? fallbackClose : line.open;
    if (reference <= 0.0 || line.high <= 0.0 || line.low <= 0.0 || line.high < line.low) {
        return -1.0;
    }
    return (line.high - line.low) / reference * 100.0;
}

int liquidityScore(double amount) {
    if (amount >= 1'000'000'000.0) return 15;
    if (amount >= 500'000'000.0) return 14;
    if (amount >= 200'000'000.0) return 12;
    if (amount >= 100'000'000.0) return 10;
    if (amount >= 50'000'000.0) return 7;
    if (amount >= 20'000'000.0) return 4;
    return 0;
}

int amplitudeScore(double amplitude) {
    if (amplitude >= 1.2 && amplitude <= 4.5) return 20;
    if (amplitude >= 0.8 && amplitude < 1.2) return 15;
    if (amplitude > 4.5 && amplitude <= 6.5) return 13;
    if (amplitude >= 0.5) return 7;
    return 0;
}

std::string formatOne(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}

}  // namespace

StockScreeningResult evaluateT0Suitability(const Quote& quote,
                                           const std::vector<KLine>& dailyLines) {
    StockScreeningResult result;
    result.quote = quote;
    result.grade = "数据不足";

    std::vector<KLine> valid;
    valid.reserve(dailyLines.size());
    for (const auto& line : dailyLines) {
        if (line.close > 0.0 && line.high > 0.0 && line.low > 0.0 &&
            line.high >= line.low) {
            valid.push_back(line);
        }
    }
    if (quote.price <= 0.0 || quote.previousClose <= 0.0 || valid.size() < 15) {
        result.reason = "实时价格或近20日有效日线不足";
        return result;
    }
    if (valid.size() > 20) {
        valid.erase(valid.begin(), valid.end() - 20);
    }

    double amplitudeSum = 0.0;
    int amplitudeCount = 0;
    int opportunityDays = 0;
    int reversalCount = 0;
    int returnCount = 0;
    int previousDirection = 0;
    for (size_t index = 0; index < valid.size(); ++index) {
        double previousClose = index > 0 ? valid[index - 1].close : valid[index].open;
        double amplitude = validAmplitude(valid[index], previousClose);
        if (amplitude >= 0.0) {
            amplitudeSum += amplitude;
            ++amplitudeCount;
            double bodyRatio = valid[index].high > valid[index].low
                                   ? std::abs(valid[index].close - valid[index].open) /
                                         (valid[index].high - valid[index].low)
                                   : 1.0;
            if (amplitude >= 1.1 && amplitude <= 8.0 && bodyRatio <= 0.78) {
                ++opportunityDays;
            }
        }
        if (index == 0 || previousClose <= 0.0) continue;
        double dayReturn = valid[index].close / previousClose - 1.0;
        int direction = dayReturn > 0.001 ? 1 : dayReturn < -0.001 ? -1 : 0;
        if (direction != 0) {
            if (previousDirection != 0) {
                ++returnCount;
                if (direction != previousDirection) ++reversalCount;
            }
            previousDirection = direction;
        }
    }
    if (amplitudeCount == 0) {
        result.reason = "日线最高价或最低价无效";
        return result;
    }

    result.averageDailyAmplitudePercent = amplitudeSum / amplitudeCount;
    result.opportunityDayPercent = static_cast<double>(opportunityDays) /
                                   amplitudeCount * 100.0;
    result.reversalPercent = returnCount > 0
                                 ? static_cast<double>(reversalCount) / returnCount * 100.0
                                 : 0.0;
    if (quote.high > 0.0 && quote.low > 0.0 && quote.high >= quote.low) {
        result.intradayRangePercent = (quote.high - quote.low) /
                                      quote.previousClose * 100.0;
    }

    size_t shortStart = valid.size() > 5 ? valid.size() - 5 : 0;
    double shortAverage = std::accumulate(valid.begin() + shortStart, valid.end(), 0.0,
                                          [](double sum, const KLine& line) {
                                              return sum + line.close;
                                          }) /
                          static_cast<double>(valid.size() - shortStart);
    double longAverage = std::accumulate(valid.begin(), valid.end(), 0.0,
                                         [](double sum, const KLine& line) {
                                             return sum + line.close;
                                         }) /
                         static_cast<double>(valid.size());
    result.trendDeviationPercent = longAverage > 0.0
                                       ? std::abs(shortAverage / longAverage - 1.0) * 100.0
                                       : 0.0;

    int score = liquidityScore(quote.amount) +
                amplitudeScore(result.averageDailyAmplitudePercent);
    score += static_cast<int>(std::lround(
        20.0 * std::clamp(result.opportunityDayPercent / 100.0, 0.0, 1.0)));
    score += static_cast<int>(std::lround(
        15.0 * std::clamp(result.reversalPercent / 100.0, 0.0, 1.0)));
    if (result.trendDeviationPercent <= 1.0) score += 15;
    else if (result.trendDeviationPercent <= 2.0) score += 12;
    else if (result.trendDeviationPercent <= 4.0) score += 7;
    else if (result.trendDeviationPercent <= 8.0) score += 2;
    if (result.intradayRangePercent >= 0.8 && result.intradayRangePercent <= 5.0) score += 5;
    else if (result.intradayRangePercent >= 0.4) score += 2;

    // 单边最低佣金会显著侵蚀小额回转收益，低成交额标的不进入适合观察区间。
    if (quote.amount < 20'000'000.0) score -= 25;
    if (result.averageDailyAmplitudePercent < 0.5) score -= 20;
    if (result.trendDeviationPercent > 10.0) score -= 20;
    if (quote.name.find("ST") != std::string::npos || quote.name.find("退") != std::string::npos) {
        score -= 25;
    }
    result.score = std::clamp(score, 0, 100);
    result.qualified = result.score >= 60;
    if (result.score >= 75) result.grade = "优先观察";
    else if (result.score >= 60) result.grade = "适合观察";
    else if (result.score >= 45) result.grade = "一般";
    else result.grade = "暂不适合";

    std::ostringstream reason;
    reason << "近20日均振幅 " << formatOne(result.averageDailyAmplitudePercent)
           << "%，机会日 " << formatOne(result.opportunityDayPercent)
           << "%，趋势偏离 " << formatOne(result.trendDeviationPercent) << "%";
    if (quote.amount < 20'000'000.0) reason << "；成交额偏低";
    else if (result.trendDeviationPercent > 8.0) reason << "；单边趋势风险较高";
    else if (result.opportunityDayPercent >= 50.0) reason << "；回转窗口较充足";
    result.reason = reason.str();
    return result;
}

const std::vector<std::string>& liquidEtfCandidates() {
    static const std::vector<std::string> values = {
        "sh510300", "sh510050", "sh510500", "sh588000", "sh588080",
        "sh512100", "sh512480", "sh512880", "sh512000", "sh512690",
        "sh515790", "sh516160", "sh513050", "sh513100", "sh513330",
        "sh513500", "sh513260", "sz159915", "sz159919", "sz159922",
        "sz159949", "sz159995", "sz159869", "sz159941", "sz159920",
        "sz159605", "sz159766",
    };
    return values;
}

const std::vector<std::string>& liquidStockCandidates() {
    static const std::vector<std::string> values = {
        "sh600519", "sh601318", "sh600036", "sh600030", "sh601166",
        "sh600276", "sh601888", "sh600900", "sh600309", "sh603259",
        "sh601012", "sh600438", "sh601899", "sh601088", "sh600887",
        "sz000001", "sz000333", "sz000858", "sz002594", "sz300750",
        "sz300059", "sz300308", "sz002415", "sz002475", "sz002714",
        "sz000063", "sz300124", "sz002230", "sz000651", "sz000725",
    };
    return values;
}

std::vector<std::string> liquidBalancedCandidates() {
    std::vector<std::string> result;
    const auto& etfs = liquidEtfCandidates();
    const auto& stocks = liquidStockCandidates();
    result.reserve(etfs.size() + stocks.size());
    size_t maximum = std::max(etfs.size(), stocks.size());
    for (size_t index = 0; index < maximum; ++index) {
        if (index < etfs.size()) result.push_back(etfs[index]);
        if (index < stocks.size()) result.push_back(stocks[index]);
    }
    return result;
}

}  // namespace ashare
