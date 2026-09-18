#include "trading_algorithm.h"

#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ashare {
namespace {

constexpr size_t kMinimumReversalSamples = 3;
constexpr int kMorningSessionMinutes = 120;
constexpr int kTradingDayMinutes = 240;
constexpr int kMorningOpenMinuteOfDay = 9 * 60 + 30;
constexpr int kAfternoonOpenMinuteOfDay = 13 * 60;

enum class T0Exposure {
    Flat,
    AwaitingBuyback,
    AwaitingSell,
};

struct T0PositionState {
    bool valid = true;
    std::string error;
    bool hasTodayTrades = false;
    long long buyQuantity = 0;
    long long sellQuantity = 0;
    long long openingBase = 0;
    long long outstandingQuantity = 0;
    int completedRounds = 0;
    double openPrice = 0.0;
    double lastClosePrice = 0.0;
    T0Exposure exposure = T0Exposure::Flat;
};

struct T0MarketState {
    int minute = 0;
    double center = 0.0;
    double vwap = 0.0;
    double sigma = 0.0;
    double relativeSigma = 0.0;
    double deviation = 0.0;
    double minimumDeviation = 0.0;
    double normalizedRegressionSlope = 0.0;
    double normalizedRecentSlope = 0.0;
    double normalizedReversalSlope = 0.0;
    double regressionFit = 0.0;
    double sessionReturn = 0.0;
    double adaptiveTrendProfitRate = 0.0;
    double limitWindowRate = 0.0;
    double marketAmount = 0.0;
    double lowerLimitPrice = 0.0;
    double upperLimitPrice = 0.0;
    bool buyReversal = false;
    bool sellReversal = false;
    bool strongUptrend = false;
    bool strongDowntrend = false;
    bool nearLowerLimit = false;
    bool nearUpperLimit = false;
};

bool finiteNonNegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

std::string tradingClockText(int minute) {
    int clockMinute = minute <= kMorningSessionMinutes
                          ? kMorningOpenMinuteOfDay + minute
                          : kAfternoonOpenMinuteOfDay + minute -
                                kMorningSessionMinutes;
    char buffer[8]{};
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", clockMinute / 60,
                  clockMinute % 60);
    return buffer;
}

void validateParameters(const T0StrategyParameters& parameters) {
    auto finiteInRange = [](double value, double low, double high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    auto finiteAtLeast = [](double value, double low) {
        return std::isfinite(value) && value >= low;
    };
    const auto& costs = parameters.transactionCosts;
    bool valid = parameters.minimumSampleCount >= kMinimumReversalSamples &&
                 parameters.sampleCount >= parameters.minimumSampleCount &&
                 parameters.shortMomentumLookback > 0 &&
                 parameters.shortMomentumLookback < parameters.sampleCount &&
                 parameters.meanReversionLookback >= kMinimumReversalSamples &&
                 parameters.meanReversionLookback <= parameters.sampleCount &&
                 parameters.reversalConfirmationLookback >=
                     kMinimumReversalSamples &&
                 parameters.reversalConfirmationLookback <=
                     parameters.meanReversionLookback &&
                 parameters.minimumReversalBarsAfterExtreme > 0 &&
                 parameters.trendRecentSlopeLookback >= kMinimumReversalSamples &&
                 parameters.trendRecentSlopeLookback < parameters.sampleCount &&
                 parameters.dailyFastPeriod > 0 &&
                 parameters.dailyFastPeriod <= parameters.dailySlowPeriod &&
                 finiteInRange(parameters.vwapCenterWeight, 0.0, 1.0) &&
                 finiteAtLeast(parameters.minimumDeviation, 0.0) &&
                 finiteAtLeast(parameters.deviationVolatilityMultiplier, 0.0) &&
                 finiteAtLeast(parameters.minimumReversalRecoveryRate, 0.0) &&
                 parameters.minimumStockReversalTicks > 0 &&
                 parameters.minimumFundReversalTicks > 0 &&
                 finiteAtLeast(parameters.maximumMeanCounterSlopePerMinute, 0.0) &&
                 finiteAtLeast(parameters.minimumTrendThreshold, 0.0) &&
                 finiteAtLeast(parameters.trendVolatilityMultiplier, 0.0) &&
                 finiteInRange(parameters.trendDirectionRatio, 0.0, 1.0) &&
                 finiteAtLeast(parameters.shortMomentumThreshold, 0.0) &&
                 finiteAtLeast(parameters.minimumRegressionSlopePerMinute, 0.0) &&
                 finiteAtLeast(parameters.regressionSlopeVolatilityMultiplier, 0.0) &&
                 finiteInRange(parameters.minimumRegressionFit, 0.0, 1.0) &&
                 finiteInRange(parameters.minimumRecentToLongSlopeRatio, 0.0, 1.0) &&
                 finiteAtLeast(parameters.trendProfitVolatilityMultiplier, 0.0) &&
                 finiteAtLeast(parameters.minimumTrendProfitRate, 0.0) &&
                 finiteAtLeast(parameters.maximumTrendProfitRate,
                               parameters.minimumTrendProfitRate) &&
                 finiteAtLeast(parameters.meanProfitMultiplier, 0.0) &&
                 finiteAtLeast(parameters.minimumMeanProfitRate, 0.0) &&
                 finiteAtLeast(parameters.maximumMeanProfitRate,
                               parameters.minimumMeanProfitRate) &&
                 finiteAtLeast(parameters.stopLossRate, 0.0) &&
                 finiteInRange(parameters.roundPositionRate, 0.0, 1.0) &&
                 parameters.roundPositionRate > 0.0 &&
                 finiteInRange(parameters.maximumRoundPositionRate,
                               parameters.roundPositionRate, 1.0) &&
                 finiteAtLeast(parameters.reentryMoveRate, 0.0) &&
                 finiteAtLeast(parameters.costSafetyMultiple, 1.0) &&
                 finiteAtLeast(parameters.minimumNetProfit, 0.0) &&
                 finiteAtLeast(parameters.minimumOrderValue, 0.0) &&
                 finiteAtLeast(parameters.minimumLiquidityAmount, 0.0) &&
                 finiteAtLeast(parameters.maximumDailyTrendDeviation, 0.0) &&
                 finiteAtLeast(parameters.sessionTrendGuardRate, 0.0) &&
                 finiteAtLeast(parameters.minimumLimitWindowRate, 0.0) &&
                 parameters.minimumLimitWindowRate > 0.0 &&
                 finiteAtLeast(parameters.maximumLimitWindowRate,
                               parameters.minimumLimitWindowRate) &&
                 finiteAtLeast(parameters.limitWindowVolatilityMultiplier, 0.0) &&
                 finiteAtLeast(parameters.trendLimitWindowMultiplier, 1.0) &&
                 finiteInRange(parameters.standardPriceLimitRate, 0.0, 1.0) &&
                 parameters.standardPriceLimitRate > 0.0 &&
                 finiteInRange(parameters.riskWarningPriceLimitRate, 0.0, 1.0) &&
                 parameters.riskWarningPriceLimitRate > 0.0 &&
                 finiteInRange(parameters.growthBoardPriceLimitRate, 0.0, 1.0) &&
                 parameters.growthBoardPriceLimitRate > 0.0 &&
                 finiteInRange(parameters.beijingPriceLimitRate, 0.0, 1.0) &&
                 parameters.beijingPriceLimitRate > 0.0 &&
                 finiteInRange(parameters.priceLimitEmergencyBufferRate, 0.0, 0.1) &&
                 finiteAtLeast(parameters.stockPriceTick, 0.0) &&
                 parameters.stockPriceTick > 0.0 &&
                 finiteAtLeast(parameters.fundPriceTick, 0.0) &&
                 parameters.fundPriceTick > 0.0 &&
                 finiteAtLeast(parameters.externalCostChangeTolerance, 0.0) &&
                 parameters.lotSize > 0 && parameters.maximumRoundsPerDay > 0 &&
                 parameters.entryStartMinute >= 0 &&
                 parameters.entryStartMinute <= parameters.middayEntryBlockStartMinute &&
                 parameters.middayEntryBlockStartMinute <=
                     parameters.middayEntryBlockEndMinute &&
                 parameters.middayEntryBlockEndMinute <= parameters.entryEndMinute &&
                 parameters.entryEndMinute < parameters.forceCloseMinute &&
                 parameters.forceCloseMinute <= kTradingDayMinutes &&
                 finiteAtLeast(costs.commissionRate, 0.0) &&
                 finiteAtLeast(costs.minimumCommission, 0.0) &&
                 finiteAtLeast(costs.stockStampDutyRate, 0.0) &&
                 finiteAtLeast(costs.stockTransferFeeRate, 0.0) &&
                 finiteAtLeast(costs.slippageRatePerSide, 0.0) &&
                 finiteAtLeast(costs.volatilitySlippageMultiplier, 0.0) &&
                 finiteAtLeast(costs.orderParticipationSlippageMultiplier, 0.0) &&
                 finiteAtLeast(costs.maximumSlippageRatePerSide,
                               costs.slippageRatePerSide);
    if (!valid) {
        throw std::invalid_argument("T0 strategy parameters are inconsistent");
    }
}

double normalizedSlope(const std::vector<KLine>& minute, size_t begin,
                       size_t count) {
    double mean = 0.0;
    for (size_t index = begin; index < begin + count; ++index) {
        mean += minute[index].close;
    }
    mean /= static_cast<double>(count);
    if (mean <= 0.0) {
        return 0.0;
    }
    const double xMean = static_cast<double>(count - 1) / 2.0;
    double covariance = 0.0;
    double xVariance = 0.0;
    for (size_t offset = 0; offset < count; ++offset) {
        const double xDifference = static_cast<double>(offset) - xMean;
        covariance += xDifference * (minute[begin + offset].close - mean);
        xVariance += xDifference * xDifference;
    }
    return xVariance > 0.0 ? covariance / xVariance / mean : 0.0;
}

double priceLimitRate(const Quote& quote,
                      const T0StrategyParameters& parameters) {
    if (isFundLikeSymbol(quote.symbol) &&
        (quote.name.find("科创") != std::string::npos ||
         quote.name.find("创业板") != std::string::npos ||
         quote.name.find("双创") != std::string::npos)) {
        return parameters.growthBoardPriceLimitRate;
    }
    if (quote.symbol.rfind("bj", 0) == 0) {
        return parameters.beijingPriceLimitRate;
    }
    if (quote.symbol.rfind("sh688", 0) == 0 ||
        quote.symbol.rfind("sz30", 0) == 0) {
        return parameters.growthBoardPriceLimitRate;
    }
    std::string upperName = quote.name;
    std::transform(upperName.begin(), upperName.end(), upperName.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::toupper(character));
                   });
    if (upperName.find("ST") != std::string::npos) {
        return parameters.riskWarningPriceLimitRate;
    }
    return parameters.standardPriceLimitRate;
}

void applyPriceLimitState(const Quote& quote,
                          const T0StrategyParameters& parameters,
                          T0MarketState& market) {
    if (quote.previousClose <= 0.0 || !std::isfinite(quote.previousClose)) {
        return;
    }
    const double tick = isFundLikeSymbol(quote.symbol)
                            ? parameters.fundPriceTick
                            : parameters.stockPriceTick;
    const double limitRate = priceLimitRate(quote, parameters);
    market.upperLimitPrice =
        std::round(quote.previousClose * (1.0 + limitRate) / tick) * tick;
    market.lowerLimitPrice =
        std::round(quote.previousClose * (1.0 - limitRate) / tick) * tick;
    const double buffer = std::max(
        tick, quote.previousClose * parameters.priceLimitEmergencyBufferRate);
    market.nearUpperLimit = quote.price >= market.upperLimitPrice - buffer;
    market.nearLowerLimit = quote.price <= market.lowerLimitPrice + buffer;
}

T0PositionState inspectPosition(const Holding& holding,
                                const T0StrategyParameters& parameters) {
    // 每次计算信号前都从真实持仓和当日成交反推状态，避免只依赖内存状态造成漂移。
    T0PositionState state;
    if (holding.quantity < 0 || !finiteNonNegative(holding.cost) ||
        (holding.quantity > 0 && holding.cost <= 0.0)) {
        state.valid = false;
        state.error = "当前持仓数量或成本无效";
        return state;
    }
    const std::string currentDate = currentDateToken();
    state.hasTodayTrades = holding.todayTradeDate == currentDate;
    if (state.hasTodayTrades) {
        state.buyQuantity = holding.todayBuyQuantity;
        state.sellQuantity = holding.todaySellQuantity;
        state.completedRounds = holding.todayT0CompletedRounds;
        state.lastClosePrice = holding.todayT0LastClosePrice;
        bool validFields = state.buyQuantity >= 0 && state.sellQuantity >= 0 &&
                           state.completedRounds >= 0 &&
                           finiteNonNegative(holding.todayBuyPrice) &&
                           finiteNonNegative(holding.todaySellPrice) &&
                           finiteNonNegative(holding.todayFees) &&
                           finiteNonNegative(holding.todayT0OpenPrice) &&
                           finiteNonNegative(holding.todayT0LastClosePrice) &&
                           (state.buyQuantity == 0 || holding.todayBuyPrice > 0.0) &&
                           (state.sellQuantity == 0 || holding.todaySellPrice > 0.0);
        if (!validFields) {
            state.valid = false;
            state.error = "当日成交字段包含负数、无效价格或缺失均价";
            return state;
        }
    }

    if (holding.tradeBaseDate == currentDate) {
        bool validBase = holding.tradeBaseQuantity >= 0 &&
                         finiteNonNegative(holding.tradeBaseCost) &&
                         (holding.tradeBaseQuantity == 0 || holding.tradeBaseCost > 0.0);
        if (!validBase) {
            state.valid = false;
            state.error = "当日持仓基线无效";
            return state;
        }
        const long long expectedQuantity = holding.tradeBaseQuantity +
                                           state.buyQuantity - state.sellQuantity;
        if (expectedQuantity != holding.quantity) {
            state.valid = false;
            state.error = "检测到外部持仓变动：当前数量与持仓基线及已登记成交不一致";
            return state;
        }
        if (!state.hasTodayTrades && holding.quantity > 0 &&
            std::fabs(holding.cost - holding.tradeBaseCost) >
                parameters.externalCostChangeTolerance) {
            state.valid = false;
            state.error = "检测到外部持仓变动：当前成本与当日持仓基线不一致";
            return state;
        }
        state.openingBase = holding.tradeBaseQuantity;
    } else {
        state.openingBase = holding.quantity - state.buyQuantity + state.sellQuantity;
    }
    if (state.openingBase < 0) {
        state.valid = false;
        state.error = "按当日成交反推的期初持仓小于 0";
        return state;
    }
    if (!state.hasTodayTrades) {
        return state;
    }
    long long outstanding = state.buyQuantity - state.sellQuantity;
    if (outstanding > 0) {
        state.exposure = T0Exposure::AwaitingSell;
        state.outstandingQuantity = outstanding;
        state.openPrice = holding.todayT0OpenPrice > 0.0
                              ? holding.todayT0OpenPrice
                              : holding.todayBuyPrice;
    } else if (outstanding < 0) {
        state.exposure = T0Exposure::AwaitingBuyback;
        state.outstandingQuantity = -outstanding;
        state.openPrice = holding.todayT0OpenPrice > 0.0
                              ? holding.todayT0OpenPrice
                              : holding.todaySellPrice;
        if (state.outstandingQuantity > state.openingBase) {
            state.valid = false;
            state.error = "待回补卖出数量超过期初底仓";
            return state;
        }
    } else if (holding.todayT0OpenPrice > 0.0) {
        state.valid = false;
        state.error = "成交已闭合但仍保留未闭合腿开仓价";
        return state;
    }

    if (state.exposure != T0Exposure::Flat) {
        if (state.openPrice <= 0.0 || !std::isfinite(state.openPrice)) {
            state.valid = false;
            state.error = "未闭合腿缺少有效开仓价";
            return state;
        }
        if (state.outstandingQuantity % parameters.lotSize != 0) {
            state.valid = false;
            state.error = "未闭合腿数量不是完整交易手，无法保证等量平仓";
            return state;
        }
    }
    return state;
}

std::optional<T0MarketState> analyzeMarket(const Quote& quote,
                                          const std::vector<KLine>& minute,
                                          const T0StrategyParameters& parameters,
                                          std::string& error) {
    auto position = tradingMinutePosition(minute.back().date);
    if (!position) {
        error = "分钟时间不在连续竞价时段";
        return std::nullopt;
    }

    T0MarketState market;
    market.minute = position->minute;
    market.marketAmount = std::max(0.0, quote.amount);
    applyPriceLimitState(quote, parameters, market);
    const size_t sampleCount = parameters.sampleCount;
    const size_t begin = minute.size() - sampleCount;
    double total = 0.0;
    double weightedTotal = 0.0;
    double volumeTotal = 0.0;
    for (size_t index = begin; index < minute.size(); ++index) {
        double close = minute[index].close;
        if (close <= 0.0 || !std::isfinite(close)) {
            error = "分钟价格数据无效";
            return std::nullopt;
        }
        total += close;
        double volume = std::max(0.0, minute[index].volume);
        weightedTotal += close * volume;
        volumeTotal += volume;
    }
    double mean = total / static_cast<double>(sampleCount);
    market.vwap = volumeTotal > 0.0 ? weightedTotal / volumeTotal : mean;
    market.center = mean * (1.0 - parameters.vwapCenterWeight) +
                    market.vwap * parameters.vwapCenterWeight;

    // 对最近 N 分钟做一元线性回归。斜率判断方向，R 方过滤方向频繁变化的噪声震荡。
    double squaredPriceDifferences = 0.0;
    double xMean = static_cast<double>(sampleCount - 1) / 2.0;
    double covariance = 0.0;
    double xVariance = 0.0;
    int risingSteps = 0;
    int fallingSteps = 0;
    for (size_t offset = 0; offset < sampleCount; ++offset) {
        double close = minute[begin + offset].close;
        double priceDifference = close - mean;
        double xDifference = static_cast<double>(offset) - xMean;
        squaredPriceDifferences += priceDifference * priceDifference;
        covariance += xDifference * priceDifference;
        xVariance += xDifference * xDifference;
        if (offset > 0) {
            double previous = minute[begin + offset - 1].close;
            if (close > previous) ++risingSteps;
            else if (close < previous) ++fallingSteps;
        }
    }
    market.sigma = std::sqrt(squaredPriceDifferences / static_cast<double>(sampleCount));
    market.relativeSigma = market.center > 0.0 ? market.sigma / market.center : 0.0;
    market.deviation = market.center > 0.0 ? quote.price / market.center - 1.0 : 0.0;
    market.minimumDeviation = std::max(
        parameters.minimumDeviation,
        market.relativeSigma * parameters.deviationVolatilityMultiplier);

    double slope = xVariance > 0.0 ? covariance / xVariance : 0.0;
    market.normalizedRegressionSlope = mean > 0.0 ? slope / mean : 0.0;
    market.normalizedRecentSlope = normalizedSlope(
        minute, minute.size() - parameters.trendRecentSlopeLookback,
        parameters.trendRecentSlopeLookback);
    market.normalizedReversalSlope = normalizedSlope(
        minute, minute.size() - parameters.reversalConfirmationLookback,
        parameters.reversalConfirmationLookback);
    if (xVariance > 0.0 && squaredPriceDifferences > 0.0) {
        market.regressionFit = std::clamp(
            covariance * covariance / (xVariance * squaredPriceDifferences), 0.0, 1.0);
    }

    double firstPrice = minute[begin].close;
    double trendReturn = firstPrice > 0.0 ? quote.price / firstPrice - 1.0 : 0.0;
    // 首笔价格容易受集合竞价和开盘瞬时成交影响，前五分钟均价更适合作为盘中方向基准。
    const size_t sessionReferenceCount = std::min<size_t>(5, minute.size());
    double sessionReference = 0.0;
    for (size_t index = 0; index < sessionReferenceCount; ++index) {
        sessionReference += minute[index].close;
    }
    sessionReference /= static_cast<double>(sessionReferenceCount);
    market.sessionReturn = sessionReference > 0.0
                               ? quote.price / sessionReference - 1.0 : 0.0;
    size_t shortBaseIndex = minute.size() - 1 - parameters.shortMomentumLookback;
    double shortBase = minute[shortBaseIndex].close;
    double shortReturn = shortBase > 0.0 ? quote.price / shortBase - 1.0 : 0.0;
    double directionSamples = static_cast<double>(sampleCount - 1);
    double risingRatio = static_cast<double>(risingSteps) / directionSamples;
    double fallingRatio = static_cast<double>(fallingSteps) / directionSamples;
    double trendThreshold = std::max(
        parameters.minimumTrendThreshold,
        market.relativeSigma * parameters.trendVolatilityMultiplier);
    double slopeThreshold = std::max(
        parameters.minimumRegressionSlopePerMinute,
        market.relativeSigma / directionSamples *
            parameters.regressionSlopeVolatilityMultiplier);
    bool regressionUp = market.normalizedRegressionSlope >= slopeThreshold &&
                        market.regressionFit >= parameters.minimumRegressionFit;
    bool regressionDown = market.normalizedRegressionSlope <= -slopeThreshold &&
                          market.regressionFit >= parameters.minimumRegressionFit;
    bool recentTrendUp = market.normalizedRecentSlope >=
                         std::max(slopeThreshold,
                                  market.normalizedRegressionSlope *
                                      parameters.minimumRecentToLongSlopeRatio);
    bool recentTrendDown = market.normalizedRecentSlope <=
                           std::min(-slopeThreshold,
                                    market.normalizedRegressionSlope *
                                        parameters.minimumRecentToLongSlopeRatio);
    market.strongUptrend = regressionUp && recentTrendUp &&
                           trendReturn >= trendThreshold &&
                           risingRatio >= parameters.trendDirectionRatio &&
                           shortReturn >= parameters.shortMomentumThreshold &&
                           quote.price > market.vwap;
    market.strongDowntrend = regressionDown && recentTrendDown &&
                             trendReturn <= -trendThreshold &&
                             fallingRatio >= parameters.trendDirectionRatio &&
                             shortReturn <= -parameters.shortMomentumThreshold &&
                             quote.price < market.vwap;

    market.adaptiveTrendProfitRate = std::clamp(
        market.relativeSigma * parameters.trendProfitVolatilityMultiplier,
        parameters.minimumTrendProfitRate, parameters.maximumTrendProfitRate);
    double windowRate = std::clamp(
        market.relativeSigma * parameters.limitWindowVolatilityMultiplier,
        parameters.minimumLimitWindowRate, parameters.maximumLimitWindowRate);
    if (market.strongUptrend || market.strongDowntrend) {
        windowRate = std::clamp(windowRate * parameters.trendLimitWindowMultiplier,
                                parameters.minimumLimitWindowRate,
                                parameters.maximumLimitWindowRate);
    }
    market.limitWindowRate = windowRate;

    // 低价 ETF 经常连续数分钟同价，严格的三点拐点会漏掉真实反转。
    // 在短窗口内先确认价格到达偏离阈值，再要求至少恢复一个最小价位且短斜率转向。
    const size_t reversalBegin = minute.size() - parameters.meanReversionLookback;
    double recentLow = minute[reversalBegin].close;
    double recentHigh = recentLow;
    size_t recentLowIndex = reversalBegin;
    size_t recentHighIndex = reversalBegin;
    for (size_t index = reversalBegin + 1; index < minute.size(); ++index) {
        if (minute[index].close <= recentLow) {
            recentLow = minute[index].close;
            recentLowIndex = index;
        }
        if (minute[index].close >= recentHigh) {
            recentHigh = minute[index].close;
            recentHighIndex = index;
        }
    }
    double previous = minute[minute.size() - 2].close;
    double priceTick = isFundLikeSymbol(quote.symbol)
                           ? parameters.fundPriceTick : parameters.stockPriceTick;
    int minimumRecoveryTicks = isFundLikeSymbol(quote.symbol)
                                   ? parameters.minimumFundReversalTicks
                                   : parameters.minimumStockReversalTicks;
    double recovery = std::max(priceTick * minimumRecoveryTicks,
                               quote.price * parameters.minimumReversalRecoveryRate);
    size_t currentIndex = minute.size() - 1;
    bool lowConfirmationElapsed =
        currentIndex >= recentLowIndex +
                            static_cast<size_t>(parameters.minimumReversalBarsAfterExtreme);
    bool highConfirmationElapsed =
        currentIndex >= recentHighIndex +
                            static_cast<size_t>(parameters.minimumReversalBarsAfterExtreme);
    bool reachedLowExcursion = recentLow <=
                               market.center * (1.0 - market.minimumDeviation);
    bool reachedHighExcursion = recentHigh >=
                                market.center * (1.0 + market.minimumDeviation);
    market.buyReversal = reachedLowExcursion && lowConfirmationElapsed &&
                         quote.price >= previous &&
                         quote.price - recentLow >= recovery &&
                         market.normalizedReversalSlope > 0.0;
    market.sellReversal = reachedHighExcursion && highConfirmationElapsed &&
                          quote.price <= previous &&
                          recentHigh - quote.price >= recovery &&
                          market.normalizedReversalSlope < 0.0;
    return market;
}

void applyLimitWindow(TradingSignal& signal, const T0MarketState& market) {
    // 限价窗口跟随近期波动率变化，趋势行情适当放宽，但始终受参数上下限约束。
    signal.windowLow = signal.currentPrice * (1.0 - market.limitWindowRate);
    signal.windowHigh = signal.currentPrice * (1.0 + market.limitWindowRate);
    if (market.lowerLimitPrice > 0.0) {
        signal.windowLow = std::max(signal.windowLow, market.lowerLimitPrice);
    }
    if (market.upperLimitPrice > 0.0) {
        signal.windowHigh = std::min(signal.windowHigh, market.upperLimitPrice);
    }
    if (signal.windowLow > signal.windowHigh) {
        signal.windowLow = signal.currentPrice;
        signal.windowHigh = signal.currentPrice;
    }
}

TradingSignal evaluateClose(const TradingSignal& baseSignal,
                            const T0PositionState& position,
                            const T0MarketState& market,
                            const T0StrategyParameters& parameters,
                            bool fundLike) {
    TradingSignal signal = baseSignal;
    // 买回已卖出的底仓和卖出日内新增仓共用同一套收益、止损及尾盘闭合判断。
    const bool closeWithBuy = position.exposure == T0Exposure::AwaitingBuyback;
    const double buyPrice = closeWithBuy ? signal.currentPrice : position.openPrice;
    const double sellPrice = closeWithBuy ? position.openPrice : signal.currentPrice;
    T0CostEstimate costs = estimateT0RoundTripCost(
        buyPrice, sellPrice, position.outstandingQuantity, fundLike,
        parameters.transactionCosts,
        T0CostContext{market.relativeSigma, market.marketAmount});
    double grossProfit = (sellPrice - buyPrice) *
                         static_cast<double>(position.outstandingQuantity);
    signal.suggestedQuantity = position.outstandingQuantity;
    signal.estimatedCost = costs.total;
    signal.expectedGrossProfit = grossProfit;
    signal.expectedNetProfit = grossProfit - costs.total;

    double profitRate = position.openPrice > 0.0
                            ? grossProfit /
                                  (position.openPrice * position.outstandingQuantity)
                            : 0.0;
    double meanOpportunity = market.center > 0.0
                                 ? std::fabs(position.openPrice / market.center - 1.0)
                                 : 0.0;
    double adaptiveMeanProfitRate = std::clamp(
        meanOpportunity * parameters.meanProfitMultiplier,
        parameters.minimumMeanProfitRate, parameters.maximumMeanProfitRate);
    bool favorableTrend = closeWithBuy ? market.strongDowntrend : market.strongUptrend;
    bool adverseTrend = closeWithBuy ? market.strongUptrend : market.strongDowntrend;
    // T0 的首要目标是尽快闭合敞口。均值回归开仓后即使行情转为顺势，
    // 也不能临时抬高止盈门槛，否则已经覆盖成本的利润可能重新变成止损。
    double effectiveProfitRate = favorableTrend
                                     ? std::min(adaptiveMeanProfitRate,
                                                market.adaptiveTrendProfitRate)
                                     : adaptiveMeanProfitRate;
    bool takeProfit = profitRate >= effectiveProfitRate &&
                      signal.expectedNetProfit >= parameters.minimumNetProfit;
    bool trendTakeProfit = takeProfit && favorableTrend;
    bool meanTakeProfit = takeProfit && !favorableTrend;
    bool priceStop = closeWithBuy
                         ? signal.currentPrice >=
                               position.openPrice * (1.0 + parameters.stopLossRate)
                         : signal.currentPrice <=
                               position.openPrice * (1.0 - parameters.stopLossRate);
    bool stopLoss = adverseTrend || priceStop;
    bool forceClose = market.minute >= parameters.forceCloseMinute;
    bool priceLimitEmergency = market.nearUpperLimit || market.nearLowerLimit;
    if (!meanTakeProfit && !trendTakeProfit && !stopLoss && !forceClose &&
        !priceLimitEmergency) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2) << "已有 "
                << position.outstandingQuantity
                << (closeWithBuy ? " 股待回补，禁止继续卖出；当前成本后空间 "
                                 : " 股待卖出，禁止继续买入；当前成本后空间 ")
                << signal.expectedNetProfit << " 元";
        signal.message = message.str();
        return signal;
    }

    signal.type = closeWithBuy ? SignalType::Buy : SignalType::Sell;
    applyLimitWindow(signal, market);
    if (priceLimitEmergency) {
        const bool upper = market.nearUpperLimit;
        if (closeWithBuy) {
            signal.message = upper
                                 ? "涨停应急：停止等待回落，立即提交等量买回以消除卖出敞口"
                                 : "跌停应急：立即提交等量买回，锁定已卖出底仓的价格空间";
        } else {
            signal.message = upper
                                 ? "涨停应急：立即提交等量卖出，锁定日内买入腿收益"
                                 : "跌停应急：停止等待反弹，立即提交等量卖出以控制买入敞口";
        }
    } else if (forceClose && !meanTakeProfit && !trendTakeProfit && !stopLoss) {
        signal.message = tradingClockText(parameters.forceCloseMinute) +
                         (closeWithBuy ? " 后强制等量买回，消除当日卖出敞口"
                                       : " 后强制等量卖出，消除当日买入敞口");
    } else if (stopLoss && !meanTakeProfit && !trendTakeProfit) {
        if (signal.expectedNetProfit > 0.0) {
            signal.message = closeWithBuy
                                 ? "上涨趋势反向确认，保护性买回锁定已有收益"
                                 : "下跌趋势反向确认，保护性卖出锁定已有收益";
        } else {
            signal.message = closeWithBuy
                                 ? "上涨趋势反向确认，等量买回止损，防止卖出敞口继续扩大"
                                 : "下跌趋势反向确认，等量卖出止损，防止买入敞口继续扩大";
        }
    } else if (trendTakeProfit) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << (closeWithBuy ? "单边下跌延续，达到自适应目标 "
                                 : "单边上涨延续，达到自适应目标 ")
                << effectiveProfitRate * 100.0
                << (closeWithBuy ? "% 后快速买回止盈" : "% 后快速卖出止盈");
        signal.message = message.str();
    } else {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << (closeWithBuy ? "价格回落达到自适应回归目标 "
                                 : "价格反弹达到自适应回归目标 ")
                << adaptiveMeanProfitRate * 100.0
                << (closeWithBuy ? "% ，等量买回止盈" : "% ，等量卖出止盈");
        signal.message = message.str();
    }
    return signal;
}

std::optional<std::pair<double, double>> dailyAverages(
    const std::vector<KLine>& daily, const T0StrategyParameters& parameters,
    std::string& error) {
    double fast = 0.0;
    double slow = 0.0;
    size_t slowBegin = daily.size() - parameters.dailySlowPeriod;
    size_t fastBegin = daily.size() - parameters.dailyFastPeriod;
    for (size_t index = slowBegin; index < daily.size(); ++index) {
        double close = daily[index].close;
        if (close <= 0.0 || !std::isfinite(close)) {
            error = "日线数据无效";
            return std::nullopt;
        }
        slow += close;
        if (index >= fastBegin) fast += close;
    }
    return std::pair<double, double>{
        fast / static_cast<double>(parameters.dailyFastPeriod),
        slow / static_cast<double>(parameters.dailySlowPeriod)};
}

TradingSignal evaluateEntry(const TradingSignal& baseSignal,
                            const std::vector<KLine>& daily,
                            const T0PositionState& position,
                            const T0MarketState& market,
                            const T0StrategyParameters& parameters,
                            bool fundLike) {
    TradingSignal signal = baseSignal;
    if (market.nearUpperLimit || market.nearLowerLimit) {
        signal.message = market.nearUpperLimit
                             ? "接近涨停价，停止新开 T0 敞口，避免追涨后无法及时回转"
                             : "接近跌停价，停止新开 T0 敞口，避免流动性不足导致无法平仓";
        return signal;
    }
    long long standardRoundQuantity = static_cast<long long>(
        static_cast<double>(position.openingBase) * parameters.roundPositionRate);
    standardRoundQuantity = (standardRoundQuantity / parameters.lotSize) * parameters.lotSize;
    long long maximumRoundQuantity = static_cast<long long>(
        static_cast<double>(position.openingBase) * parameters.maximumRoundPositionRate);
    maximumRoundQuantity = (maximumRoundQuantity / parameters.lotSize) * parameters.lotSize;
    long long minimumValueQuantity = signal.currentPrice > 0.0
        ? static_cast<long long>(std::ceil(
              parameters.minimumOrderValue / signal.currentPrice /
              static_cast<double>(parameters.lotSize))) * parameters.lotSize
        : 0;
    if (minimumValueQuantity > standardRoundQuantity &&
        minimumValueQuantity <= maximumRoundQuantity) {
        standardRoundQuantity = minimumValueQuantity;
    }
    int completedRounds = position.completedRounds;
    if (standardRoundQuantity >= parameters.lotSize && completedRounds == 0) {
        completedRounds = static_cast<int>(
            std::min(position.buyQuantity, position.sellQuantity) / standardRoundQuantity);
    }
    if (completedRounds >= parameters.maximumRoundsPerDay) {
        signal.message = "当日已完成 " + std::to_string(parameters.maximumRoundsPerDay) +
                         " 轮 T0，停止新开仓以控制交易频率和费用";
        return signal;
    }
    long long remainingSellableBase = std::max(
        0LL, position.openingBase - position.sellQuantity);
    long long quantity = std::min(
        standardRoundQuantity,
        (remainingSellableBase / parameters.lotSize) * parameters.lotSize);
    if (quantity < parameters.lotSize) {
        signal.message = standardRoundQuantity < parameters.lotSize
                             ? "现有底仓不足以按单轮仓位比例生成一手委托"
                             : "当日可用于 T0 的原有底仓已经不足一手";
        return signal;
    }
    signal.suggestedQuantity = quantity;
    if (static_cast<double>(quantity) * signal.currentPrice < parameters.minimumOrderValue) {
        signal.message = "单轮成交金额过小，最低佣金占比过高";
        return signal;
    }
    if (market.minute < parameters.entryStartMinute) {
        signal.message = tradingClockText(parameters.entryStartMinute) +
                         " 前不新开 T0 敞口，等待开盘波动稳定";
        return signal;
    }
    if (market.minute >= parameters.middayEntryBlockStartMinute &&
        market.minute <= parameters.middayEntryBlockEndMinute) {
        signal.message = "午间收盘前后不新开 T0 敞口";
        return signal;
    }
    if (market.minute > parameters.entryEndMinute) {
        signal.message = tradingClockText(parameters.entryEndMinute + 1) +
                         " 后停止新开 T0 敞口，为 " +
                         tradingClockText(parameters.forceCloseMinute) + " 平仓预留时间";
        return signal;
    }
    if (market.marketAmount > 0.0 &&
        market.marketAmount < parameters.minimumLiquidityAmount) {
        signal.message = "当日成交额低于策略流动性门槛，暂不进行 T0 判断";
        return signal;
    }

    std::string averageError;
    auto averages = dailyAverages(daily, parameters, averageError);
    if (!averages) {
        signal.message = averageError;
        return signal;
    }
    // 趋势信号已经通过斜率和拟合度确认；非趋势行情仍按偏离均值后的反转处理。
    bool trendSetup = market.strongUptrend || market.strongDowntrend;
    bool buySetup = market.strongUptrend || (!trendSetup && market.buyReversal);
    bool sellSetup = market.strongDowntrend || (!trendSetup && market.sellReversal);
    if (!buySetup && !sellSetup) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "等待价格偏离均值 " << market.minimumDeviation * 100.0
                << "% 并出现反转确认；当前回归斜率 "
                << market.normalizedRegressionSlope * 100.0
                << "%/分钟，最近斜率 "
                << market.normalizedRecentSlope * 100.0 << "%/分钟";
        signal.message = message.str();
        return signal;
    }

    bool counterSlopeBuy = !trendSetup && buySetup &&
                           market.normalizedRegressionSlope <
                               -parameters.maximumMeanCounterSlopePerMinute;
    bool counterSlopeSell = !trendSetup && sellSetup &&
                            market.normalizedRegressionSlope >
                                parameters.maximumMeanCounterSlopePerMinute;
    if (counterSlopeBuy || counterSlopeSell) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(3)
                << "反转已出现，但中窗斜率仍为 "
                << market.normalizedRegressionSlope * 100.0
                << "%/分钟，等待趋势进一步减弱后再"
                << (counterSlopeBuy ? "低位买入" : "高位卖出");
        signal.message = message.str();
        return signal;
    }

    bool counterSessionBuy = !trendSetup && buySetup &&
                             market.sessionReturn <= -parameters.sessionTrendGuardRate;
    bool counterSessionSell = !trendSetup && sellSetup &&
                              market.sessionReturn >= parameters.sessionTrendGuardRate;
    if (counterSessionBuy || counterSessionSell) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "盘中累计涨跌 " << market.sessionReturn * 100.0
                << "% ，等待单边方向结束后再进行"
                << (counterSessionBuy ? "低位买入" : "高位卖出");
        signal.message = message.str();
        return signal;
    }

    // 均值回归只做与日线方向一致的一侧：日线偏空时不接下跌中的低点，
    // 日线偏多时不提前卖出上涨中的底仓。顺势信号仍由分钟回归斜率独立确认。
    double dailyTrend = averages->second > 0.0
                            ? averages->first / averages->second - 1.0
                            : 0.0;
    bool counterTrendBuy = !trendSetup && buySetup &&
                           dailyTrend < -parameters.maximumDailyTrendDeviation;
    bool counterTrendSell = !trendSetup && sellSetup &&
                            dailyTrend > parameters.maximumDailyTrendDeviation;
    if (counterTrendBuy || counterTrendSell) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "日线快慢均线偏离 " << dailyTrend * 100.0
                << "% ，过滤逆日线方向的"
                << (counterTrendBuy ? "低位买入" : "高位卖出");
        signal.message = message.str();
        return signal;
    }

    if (completedRounds > 0 && position.lastClosePrice > 0.0) {
        bool reentryReady = market.strongUptrend
                                ? signal.currentPrice >= position.lastClosePrice *
                                      (1.0 + parameters.reentryMoveRate)
                                : market.strongDowntrend
                                      ? signal.currentPrice <= position.lastClosePrice *
                                            (1.0 - parameters.reentryMoveRate)
                                      : buySetup
                                            ? signal.currentPrice <= position.lastClosePrice *
                                                  (1.0 - parameters.reentryMoveRate)
                                            : signal.currentPrice >= position.lastClosePrice *
                                                  (1.0 + parameters.reentryMoveRate);
        if (!reentryReady) {
            std::ostringstream message;
            message << std::fixed << std::setprecision(2)
                    << "上一轮刚完成，等待价格相对平仓价再移动至少 "
                    << parameters.reentryMoveRate * 100.0 << "%";
            signal.message = message.str();
            return signal;
        }
    }

    double buyPrice = 0.0;
    double sellPrice = 0.0;
    if (market.strongUptrend) {
        buyPrice = signal.currentPrice;
        sellPrice = signal.currentPrice * (1.0 + market.adaptiveTrendProfitRate);
    } else if (market.strongDowntrend) {
        buyPrice = signal.currentPrice * (1.0 - market.adaptiveTrendProfitRate);
        sellPrice = signal.currentPrice;
    } else {
        buyPrice = buySetup ? signal.currentPrice : market.center;
        sellPrice = buySetup ? market.center : signal.currentPrice;
    }
    T0CostEstimate costs = estimateT0RoundTripCost(
        buyPrice, sellPrice, quantity, fundLike, parameters.transactionCosts,
        T0CostContext{market.relativeSigma, market.marketAmount});
    double grossProfit = (sellPrice - buyPrice) * static_cast<double>(quantity);
    double netProfit = grossProfit - costs.total;
    signal.estimatedCost = costs.total;
    signal.expectedGrossProfit = grossProfit;
    signal.expectedNetProfit = netProfit;
    if (grossProfit < costs.total * parameters.costSafetyMultiple ||
        netProfit < parameters.minimumNetProfit) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2) << "预期毛差 " << grossProfit
                << " 元，未达到完整成本 " << costs.total << " 元的 "
                << parameters.costSafetyMultiple << " 倍安全门槛";
        signal.message = message.str();
        return signal;
    }

    signal.type = buySetup ? SignalType::Buy : SignalType::Sell;
    applyLimitWindow(signal, market);
    double targetPrice = market.strongUptrend
                             ? sellPrice
                             : market.strongDowntrend ? buyPrice : market.center;
    std::ostringstream message;
    message << std::fixed << std::setprecision(2)
            << (market.strongUptrend
                    ? "线性回归确认单边上涨且短窗未走弱，顺势买入后快速卖出等量底仓"
                    : market.strongDowntrend
                          ? "线性回归确认单边下跌且短窗未走弱，顺势卖出底仓后快速等量买回"
                          : buySetup ? "低位买入后卖出等量底仓"
                                     : "高位卖出底仓后等量买回")
            << (trendSetup ? "；快速止盈目标 " : "；回归目标 ") << targetPrice;
    if (trendSetup) {
        message << "（动态目标 " << market.adaptiveTrendProfitRate * 100.0
                << "%；回归拟合度 " << market.regressionFit * 100.0 << "%）";
    }
    message << "，完整成本 " << costs.total << " 元，预期毛差 " << grossProfit
            << " 元，成本后空间 " << netProfit << " 元；第 " << completedRounds + 1
            << "/" << parameters.maximumRoundsPerDay << " 轮";
    signal.message = message.str();
    return signal;
}

class T0IntradayAlgorithm final : public TradingAlgorithm {
public:
    T0IntradayAlgorithm(std::string displayName, T0StrategyParameters parameters)
        : displayName_(std::move(displayName)), parameters_(std::move(parameters)) {
        validateParameters(parameters_);
    }

    std::string name() const override { return displayName_; }

    TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                           const std::vector<KLine>& minute,
                           const Holding* holding) const override {
        TradingSignal signal;
        signal.symbol = quote.symbol;
        signal.name = quote.name;
        signal.algorithm = name();
        signal.currentPrice = quote.price;
        signal.time = quote.time;
        if (!holding) {
            signal.message = "T0 日内回转需要已有底仓";
            return signal;
        }
        if (daily.size() < parameters_.dailySlowPeriod ||
            minute.size() < parameters_.sampleCount || quote.price <= 0.0 ||
            !std::isfinite(quote.price)) {
            signal.message = "日线或分钟样本不足";
            return signal;
        }

        T0PositionState position = inspectPosition(*holding, parameters_);
        if (!position.valid) {
            signal.message = "持仓状态校验失败：" + position.error;
            return signal;
        }
        std::string marketError;
        auto market = analyzeMarket(quote, minute, parameters_, marketError);
        if (!market) {
            signal.message = marketError;
            return signal;
        }
        bool fundLike = isFundLikeSymbol(quote.symbol);
        if (position.exposure != T0Exposure::Flat) {
            return evaluateClose(signal, position, *market, parameters_, fundLike);
        }
        return evaluateEntry(signal, daily, position, *market, parameters_, fundLike);
    }

private:
    std::string displayName_;
    T0StrategyParameters parameters_;
};

}  // namespace

T0CostEstimate estimateT0RoundTripCost(
    double buyPrice, double sellPrice, long long quantity, bool fundLike,
    const T0TransactionCostParameters& parameters) {
    return estimateT0RoundTripCost(buyPrice, sellPrice, quantity, fundLike,
                                   parameters, T0CostContext{});
}

T0CostEstimate estimateT0RoundTripCost(
    double buyPrice, double sellPrice, long long quantity, bool fundLike,
    const T0TransactionCostParameters& parameters,
    const T0CostContext& context) {
    T0CostEstimate estimate;
    if (buyPrice <= 0.0 || sellPrice <= 0.0 || quantity <= 0) return estimate;
    double buyNotional = buyPrice * static_cast<double>(quantity);
    double sellNotional = sellPrice * static_cast<double>(quantity);
    estimate.buyCommission = std::max(parameters.minimumCommission,
                                      buyNotional * parameters.commissionRate);
    estimate.sellCommission = std::max(parameters.minimumCommission,
                                       sellNotional * parameters.commissionRate);
    if (!fundLike) {
        estimate.stampDuty = sellNotional * parameters.stockStampDutyRate;
        estimate.transferFee = (buyNotional + sellNotional) *
                               parameters.stockTransferFeeRate;
    }
    const double volatility = finiteNonNegative(context.relativeVolatility)
                                  ? context.relativeVolatility
                                  : 0.0;
    const double participation = finiteNonNegative(context.marketAmount) &&
                                         context.marketAmount > 0.0
                                     ? std::max(buyNotional, sellNotional) /
                                           context.marketAmount
                                     : 0.0;
    const double maximumSlippage = std::max(
        parameters.slippageRatePerSide,
        parameters.maximumSlippageRatePerSide);
    estimate.effectiveSlippageRatePerSide = std::clamp(
        parameters.slippageRatePerSide +
            volatility * parameters.volatilitySlippageMultiplier +
            std::sqrt(std::max(0.0, participation)) *
                parameters.orderParticipationSlippageMultiplier,
        parameters.slippageRatePerSide, maximumSlippage);
    estimate.slippage = (buyNotional + sellNotional) *
                        estimate.effectiveSlippageRatePerSide;
    estimate.total = estimate.buyCommission + estimate.sellCommission +
                     estimate.stampDuty + estimate.transferFee + estimate.slippage;
    return estimate;
}

T0CostEstimate estimateT0RoundTripCost(double buyPrice, double sellPrice,
                                       long long quantity, bool fundLike) {
    return estimateT0RoundTripCost(buyPrice, sellPrice, quantity, fundLike,
                                   defaultT0StrategyParameters().transactionCosts);
}

const T0StrategyParameters& defaultT0StrategyParameters() {
    static const T0StrategyParameters parameters;
    return parameters;
}

std::unique_ptr<TradingAlgorithm> createT0TradingAlgorithm(
    const std::string& displayName, const T0StrategyParameters& parameters) {
    return std::make_unique<T0IntradayAlgorithm>(displayName, parameters);
}

}  // namespace ashare
