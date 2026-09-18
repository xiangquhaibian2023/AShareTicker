#pragma once

#include "domain.h"

#include <string>
#include <vector>

namespace ashare {

// 当前 T0 算法对候选证券的技术适配度，不代表未来收益或投资建议。
struct StockScreeningResult {
    Quote quote;
    int score = 0;
    bool qualified = false;
    double intradayRangePercent = 0.0;
    double averageDailyAmplitudePercent = 0.0;
    double opportunityDayPercent = 0.0;
    double reversalPercent = 0.0;
    double trendDeviationPercent = 0.0;
    std::string grade;
    std::string reason;
};

// 使用实时快照和最近日线评估流动性、可交易波动、均值回归特征及趋势风险。
StockScreeningResult evaluateT0Suitability(const Quote& quote,
                                           const std::vector<KLine>& dailyLines);

// 公开行情源缺少稳定的全市场排行接口，因此使用可审计的高流动性候选池。
const std::vector<std::string>& liquidEtfCandidates();
const std::vector<std::string>& liquidStockCandidates();
std::vector<std::string> liquidBalancedCandidates();

}  // namespace ashare
