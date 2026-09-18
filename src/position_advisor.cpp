#include "position_advisor.h"

#include "utils.h"
#include "technical_indicators.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace ashare {
namespace {

double averageVolumeBeforeLast(const std::vector<KLine>& lines, size_t count) {
    if (lines.size() <= count || count == 0) return 0.0;
    auto end = lines.end() - 1;
    auto begin = end - static_cast<std::ptrdiff_t>(count);
    return std::accumulate(begin, end, 0.0,
                           [](double sum, const KLine& line) { return sum + line.volume; }) /
           static_cast<double>(count);
}

double calculateAtr(const std::vector<KLine>& lines, size_t period) {
    if (lines.size() < period + 1) return 0.0;
    double total = 0.0;
    size_t start = lines.size() - period;
    for (size_t index = start; index < lines.size(); ++index) {
        double previous = lines[index - 1].close;
        double range = std::max({lines[index].high - lines[index].low,
                                 std::abs(lines[index].high - previous),
                                 std::abs(lines[index].low - previous)});
        total += range;
    }
    return total / static_cast<double>(period);
}

long long roundLot(const std::string&) {
    return 100;
}

long long floorToLot(double quantity, long long lot) {
    if (!std::isfinite(quantity) || quantity < lot) return 0;
    return static_cast<long long>(quantity / static_cast<double>(lot)) * lot;
}

double tickSize(const std::string& symbol) {
    return isFundLikeSymbol(symbol) ? 0.001 : 0.01;
}

double roundToTick(double value, double tick) {
    if (!std::isfinite(value) || value <= 0.0) return 0.0;
    return std::round(value / tick) * tick;
}

std::string formatPercent(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << value << '%';
    return out.str();
}

}  // namespace

PositionAdvice evaluatePositionAdvice(const Holding& holding,
                                      const Quote& quote,
                                      const std::vector<KLine>& dailyLines,
                                      double totalAssets,
                                      double availableCash,
                                      double correlatedExposurePercent) {
    PositionAdvice result;
    result.symbol = holding.symbol;
    result.name = quote.name;
    result.currentPrice = quote.price;
    result.cost = holding.cost;
    result.holdingQuantity = holding.quantity;
    result.quoteTime = quote.time;
    if (quote.price > 0.0 && holding.quantity > 0) {
        result.floatingProfit = (quote.price - holding.cost) * holding.quantity;
        result.floatingReturnPercent = holding.cost > 0.0
                                           ? (quote.price / holding.cost - 1.0) * 100.0
                                           : 0.0;
    }

    std::vector<KLine> lines;
    lines.reserve(dailyLines.size());
    for (const auto& line : dailyLines) {
        if (line.close > 0.0 && line.high >= line.low && line.low > 0.0 && line.volume >= 0.0) {
            lines.push_back(line);
        }
    }
    if (quote.price <= 0.0 || holding.quantity <= 0 || lines.size() < 35) {
        result.actionText = "数据不足";
        result.reason = "需要至少35根有效日K和实时价格，暂不生成操作数量";
        return result;
    }
    if (!lines.empty()) {
        lines.back().close = quote.price;
        lines.back().high = std::max(lines.back().high, quote.price);
        lines.back().low = std::min(lines.back().low, quote.price);
        if (quote.volume > 0.0) lines.back().volume = std::max(lines.back().volume, quote.volume);
    }

    result.ma5 = averageClose(lines, 5);
    result.ma10 = averageClose(lines, 10);
    result.ma20 = averageClose(lines, 20);
    double volumeAverage = averageVolumeBeforeLast(lines, 5);
    result.volumeRatio = volumeAverage > 0.0 ? lines.back().volume / volumeAverage : 0.0;
    MacdValues macd = calculateMacd(lines);
    result.macdDif = macd.dif;
    result.macdDea = macd.dea;
    result.macdHistogram = macd.histogram;
    result.atr14 = calculateAtr(lines, 14);
    if (result.atr14 <= 0.0) result.atr14 = quote.price * 0.015;

    auto last20 = lines.end() - 20;
    double recentHigh = std::max_element(last20, lines.end(), [](const KLine& left, const KLine& right) {
                            return left.high < right.high;
                        })->high;
    double support = std::min(result.ma10, result.ma20);
    double addCenter = std::min(support, quote.price - result.atr14 * 0.45);
    double resistance = std::min(recentHigh,
                                 std::max(result.ma5 + result.atr14 * 0.50,
                                          quote.price + result.atr14 * 0.50));
    double tick = tickSize(holding.symbol);
    result.addPriceLow = roundToTick(std::max(tick, addCenter - result.atr14 * 0.25), tick);
    result.addPriceHigh = roundToTick(std::min(quote.price * 0.998,
                                               addCenter + result.atr14 * 0.20), tick);
    if (result.addPriceHigh < result.addPriceLow) {
        result.addPriceHigh = result.addPriceLow;
    }
    result.reducePriceLow = roundToTick(std::max(quote.price * 1.002,
                                                  resistance - result.atr14 * 0.25), tick);
    result.reducePriceHigh = roundToTick(resistance + result.atr14 * 0.25, tick);
    if (result.reducePriceHigh < result.reducePriceLow) {
        result.reducePriceHigh = result.reducePriceLow;
    }

    long long lot = roundLot(holding.symbol);
    double addBudget = std::min(std::max(0.0, availableCash) * 0.30,
                                totalAssets > 0.0 ? totalAssets * 0.03
                                                  : quote.price * holding.quantity * 0.10);
    long long cashQuantity = floorToLot(addBudget / std::max(result.addPriceHigh, tick), lot);
    long long trancheQuantity = floorToLot(holding.quantity * 0.15, lot);
    result.addQuantity = std::min(cashQuantity, trancheQuantity);
    result.reduceQuantity = floorToLot(holding.quantity * 0.15, lot);
    if (result.reduceQuantity == 0 && holding.quantity >= lot) result.reduceQuantity = lot;

    bool bullishAlignment = quote.price >= result.ma5 && result.ma5 >= result.ma10 &&
                            result.ma10 >= result.ma20;
    bool bearishAlignment = quote.price < result.ma20 && result.ma5 < result.ma10;
    bool macdPositive = result.macdDif >= result.macdDea && result.macdHistogram >= 0.0;
    bool macdWeakening = result.macdHistogram < macd.previousHistogram;
    bool nearAddZone = quote.price <= result.addPriceHigh + result.atr14 * 0.20;
    bool nearReduceZone = quote.price >= result.reducePriceLow - result.atr14 * 0.20;
    bool concentrated = totalAssets > 0.0 &&
                        quote.price * holding.quantity / totalAssets * 100.0 >= 35.0;
    bool correlatedConcentration = correlatedExposurePercent >= 45.0;

    std::ostringstream reason;
    if (bearishAlignment && result.macdHistogram < 0.0) {
        result.action = PositionAction::Reduce;
        result.actionText = "破位减仓";
        result.confidence = 78;
        // 弱势破位时使用最近反弹窗口，不再把远端20日高点误作当前可执行价格。
        result.reducePriceLow = roundToTick(quote.price + result.atr14 * 0.25, tick);
        result.reducePriceHigh = roundToTick(quote.price + result.atr14 * 0.75, tick);
        result.addQuantity = 0;
        reason << "价格跌破MA20且MA5低于MA10，MACD位于零轴弱势区；先控制下行敞口";
    } else if ((nearReduceZone && macdWeakening) ||
               (quote.price >= result.ma20 + result.atr14 * 1.5 && result.volumeRatio >= 1.0)) {
        result.action = PositionAction::Reduce;
        result.actionText = "冲高减仓";
        result.confidence = 72;
        reason << "价格接近近20日阻力，MACD动能走弱或放量偏离MA20";
    } else if (bullishAlignment && macdPositive && nearAddZone &&
               !concentrated && !correlatedConcentration && result.addQuantity > 0) {
        result.action = PositionAction::Add;
        result.actionText = "回踩补仓";
        result.confidence = 70;
        reason << "均线多头排列，MACD保持正向，价格回到MA10/MA20支撑区";
    } else if (quote.price <= result.ma20 * 1.01 && macdPositive &&
               macd.histogram >= macd.previousHistogram && !concentrated &&
               !correlatedConcentration && result.addQuantity > 0) {
        result.action = PositionAction::Add;
        result.actionText = "企稳补仓";
        result.confidence = 64;
        reason << "价格靠近MA20且MACD柱改善，等待区间内企稳后分批处理";
    } else {
        result.action = PositionAction::Observe;
        result.actionText = "等待触发";
        result.confidence = 55;
        reason << "当前价格未同时满足趋势、量能和支撑/阻力触发条件";
    }
    if (result.volumeRatio < 0.65) reason << "；量能不足";
    else if (result.volumeRatio > 1.5) reason << "；量能明显放大";
    if (concentrated) reason << "；单一持仓超过总资产35%，暂停补仓";
    if (correlatedConcentration) {
        reason << "；同类港股科技敞口约" << formatPercent(correlatedExposurePercent)
               << "，暂停同方向补仓";
        result.addQuantity = 0;
    }
    result.reason = reason.str();
    return result;
}

}  // namespace ashare
