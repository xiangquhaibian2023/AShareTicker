#pragma once

#include "domain.h"

#include <string>
#include <vector>

namespace ashare {

enum class PositionAction {
    DataInsufficient,
    Observe,
    Add,
    Reduce,
};

struct PositionAdvice {
    std::string symbol;
    std::string name;
    PositionAction action = PositionAction::DataInsufficient;
    double currentPrice = 0.0;
    double cost = 0.0;
    long long holdingQuantity = 0;
    double floatingProfit = 0.0;
    double floatingReturnPercent = 0.0;
    double ma5 = 0.0;
    double ma10 = 0.0;
    double ma20 = 0.0;
    double volumeRatio = 0.0;
    double macdDif = 0.0;
    double macdDea = 0.0;
    double macdHistogram = 0.0;
    double atr14 = 0.0;
    double addPriceLow = 0.0;
    double addPriceHigh = 0.0;
    double reducePriceLow = 0.0;
    double reducePriceHigh = 0.0;
    long long addQuantity = 0;
    long long reduceQuantity = 0;
    int confidence = 0;
    std::string actionText;
    std::string reason;
    std::string quoteTime;
};

// 以最新价替换最后一根日线收盘价，实时更新趋势判断；日线不足时不生成交易数量。
PositionAdvice evaluatePositionAdvice(const Holding& holding,
                                      const Quote& quote,
                                      const std::vector<KLine>& dailyLines,
                                      double totalAssets,
                                      double availableCash,
                                      double correlatedExposurePercent = 0.0);

}  // namespace ashare
