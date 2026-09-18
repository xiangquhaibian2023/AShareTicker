#include "trading_algorithm.h"
#include "utils.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool nearlyEqual(double left, double right, double tolerance = 0.000001) {
    return std::fabs(left - right) <= tolerance;
}

std::vector<ashare::KLine> makeDailyLines() {
    std::vector<ashare::KLine> lines(30);
    for (size_t index = 0; index < lines.size(); ++index) {
        lines[index].date = "2026-07-01";
        lines[index].open = 10.0;
        lines[index].close = 10.0;
        lines[index].high = 10.1;
        lines[index].low = 9.9;
        lines[index].volume = 1000000.0;
    }
    return lines;
}

std::vector<ashare::KLine> makeBuyReversalMinuteLines() {
    std::vector<ashare::KLine> lines(30);
    for (size_t index = 0; index < lines.size(); ++index) {
        int totalMinute = 10 * 60 + static_cast<int>(index);
        int hour = totalMinute / 60;
        int minute = totalMinute % 60;
        char time[32]{};
        std::snprintf(time, sizeof(time), "2026-08-07 %02d:%02d", hour, minute);
        lines[index].date = time;
        lines[index].open = 10.0;
        lines[index].close = 10.0;
        lines[index].high = 10.0;
        lines[index].low = 10.0;
        lines[index].volume = 1000.0;
    }
    // 让盘中基准与当前价格接近，测试只聚焦极值恢复而不触发单边行情过滤。
    for (size_t index = 0; index < 5; ++index) {
        lines[index].open = 9.90;
        lines[index].close = 9.90;
        lines[index].high = 9.90;
        lines[index].low = 9.90;
    }
    lines[27].close = 9.84;
    lines[28].close = 9.86;
    lines[29].close = 9.90;
    return lines;
}

std::vector<ashare::KLine> makeFlatMinuteLinesEndingAt(int endHour, int endMinute,
                                                        double price) {
    std::vector<ashare::KLine> lines(30);
    int end = endHour * 60 + endMinute;
    for (size_t index = 0; index < lines.size(); ++index) {
        int totalMinute = end - static_cast<int>(lines.size() - 1 - index);
        char time[32]{};
        std::snprintf(time, sizeof(time), "2026-08-07 %02d:%02d",
                      totalMinute / 60, totalMinute % 60);
        lines[index] = ashare::KLine{time, price, price, price, price, 1000.0};
    }
    return lines;
}

void retimeMinuteLines(std::vector<ashare::KLine>& lines, int endHour, int endMinute) {
    int end = endHour * 60 + endMinute;
    for (size_t index = 0; index < lines.size(); ++index) {
        int totalMinute = end - static_cast<int>(lines.size() - 1 - index);
        char time[32]{};
        std::snprintf(time, sizeof(time), "2026-08-07 %02d:%02d",
                      totalMinute / 60, totalMinute % 60);
        lines[index].date = time;
    }
}

std::vector<ashare::KLine> makeTrendMinuteLines(bool rising, double startPrice,
                                                double step) {
    std::vector<ashare::KLine> lines(30);
    for (size_t index = 0; index < lines.size(); ++index) {
        int totalMinute = 10 * 60 + static_cast<int>(index);
        char time[32]{};
        std::snprintf(time, sizeof(time), "2026-08-07 %02d:%02d",
                      totalMinute / 60, totalMinute % 60);
        double direction = rising ? 1.0 : -1.0;
        double price = startPrice + direction * step * static_cast<double>(index);
        lines[index] = ashare::KLine{time, price, price + 0.005, price - 0.005,
                                     price, 100000.0};
    }
    return lines;
}

std::vector<ashare::KLine> makeNoisyUpwardMinuteLines() {
    std::vector<ashare::KLine> lines(30);
    for (size_t index = 0; index < lines.size(); ++index) {
        int totalMinute = 10 * 60 + static_cast<int>(index);
        char time[32]{};
        std::snprintf(time, sizeof(time), "2026-08-07 %02d:%02d",
                      totalMinute / 60, totalMinute % 60);
        double noise = index % 2 == 0 ? -0.18 : 0.18;
        double price = 10.0 + 0.012 * static_cast<double>(index) + noise;
        lines[index] = ashare::KLine{time, price, price + 0.005, price - 0.005,
                                     price, 100000.0};
    }
    return lines;
}

std::vector<ashare::KLine> makeStaleUptrendMinuteLines() {
    std::vector<ashare::KLine> lines = makeTrendMinuteLines(true, 10.0, 0.02);
    const double stalledPrice = lines[21].close;
    for (size_t index = 22; index < lines.size(); ++index) {
        lines[index].open = stalledPrice;
        lines[index].high = stalledPrice;
        lines[index].low = stalledPrice;
        lines[index].close = stalledPrice;
    }
    return lines;
}

} // namespace

int main() {
    using namespace ashare;

    const T0StrategyParameters defaults = defaultT0StrategyParameters();
    assert(defaults.sampleCount == 25);
    assert(defaults.minimumReversalBarsAfterExtreme == 2);
    assert(defaults.minimumFundReversalTicks == 2);
    assert(nearlyEqual(defaults.minimumDeviation, 0.0030));
    assert(nearlyEqual(defaults.costSafetyMultiple, 2.0));
    assert(nearlyEqual(defaults.minimumMeanProfitRate, 0.0020));
    assert(nearlyEqual(defaults.minimumRegressionFit, 0.70));
    assert(defaults.entryEndMinute == 180);
    assert(defaults.forceCloseMinute == 220);

    T0CostEstimate stockCost = estimateT0RoundTripCost(10.0, 10.1, 100, false);
    assert(nearlyEqual(stockCost.buyCommission, 5.0));
    assert(nearlyEqual(stockCost.sellCommission, 5.0));
    assert(nearlyEqual(stockCost.stampDuty, 0.505));
    assert(nearlyEqual(stockCost.transferFee, 0.0201));
    assert(nearlyEqual(stockCost.slippage, 0.402));
    assert(nearlyEqual(stockCost.total, 10.9271));

    T0CostEstimate fundCost = estimateT0RoundTripCost(10.0, 10.1, 100, true);
    assert(nearlyEqual(fundCost.stampDuty, 0.0));
    assert(nearlyEqual(fundCost.transferFee, 0.0));
    assert(nearlyEqual(fundCost.total, 10.402));

    T0CostContext calmMarket{0.001, 1000000000.0};
    T0CostContext stressedMarket{0.020, 1000000.0};
    T0CostEstimate calmCost = estimateT0RoundTripCost(
        10.0, 10.1, 10000, false, defaultT0StrategyParameters().transactionCosts,
        calmMarket);
    T0CostEstimate stressedCost = estimateT0RoundTripCost(
        10.0, 10.1, 10000, false, defaultT0StrategyParameters().transactionCosts,
        stressedMarket);
    assert(stressedCost.effectiveSlippageRatePerSide >
           calmCost.effectiveSlippageRatePerSide);
    assert(stressedCost.slippage > calmCost.slippage);
    assert(stressedCost.effectiveSlippageRatePerSide <=
           defaultT0StrategyParameters().transactionCosts.maximumSlippageRatePerSide);

    T0TransactionCostParameters freeTrading;
    freeTrading.commissionRate = 0.0;
    freeTrading.minimumCommission = 0.0;
    freeTrading.stockStampDutyRate = 0.0;
    freeTrading.stockTransferFeeRate = 0.0;
    freeTrading.slippageRatePerSide = 0.0;
    assert(nearlyEqual(estimateT0RoundTripCost(10.0, 10.1, 100, false,
                                               freeTrading).total,
                       0.0));

    assert(strategyKindFromKey("t0_intraday") == StrategyKind::T0Intraday);
    StrategyConfig config{"test_t0", "T0 测试", StrategyKind::T0Intraday};
    auto algorithm = createTradingAlgorithm(config);
    Quote quote;
    quote.symbol = "sh600000";
    quote.name = "测试股票";
    quote.price = 9.90;
    quote.amount = 100000000.0;
    quote.time = "20260807102900";
    auto daily = makeDailyLines();
    auto minute = makeBuyReversalMinuteLines();

    TradingSignal noHolding = algorithm->evaluate(quote, daily, minute, nullptr);
    assert(noHolding.type == SignalType::None);

    Holding smallHolding;
    smallHolding.symbol = "sh600000";
    smallHolding.quantity = 500;
    smallHolding.cost = 10.0;
    TradingSignal costBlocked = algorithm->evaluate(quote, daily, minute, &smallHolding);
    assert(costBlocked.type == SignalType::None);
    assert(costBlocked.message.find("成交金额过小") != std::string::npos);

    Holding sufficientHolding;
    sufficientHolding.symbol = "sh600000";
    sufficientHolding.quantity = 30000;
    sufficientHolding.cost = 10.0;
    TradingSignal buySignal = algorithm->evaluate(quote, daily, minute, &sufficientHolding);
    assert(buySignal.type == SignalType::Buy);
    assert(buySignal.suggestedQuantity == 12000);
    assert(buySignal.expectedGrossProfit >= buySignal.estimatedCost * 2.0);
    assert(buySignal.expectedNetProfit > 0.0);

    // 极值刚出现一根恢复 K 线时继续观察，避免把单根噪声误判为反转。
    auto oneBarReversal = makeBuyReversalMinuteLines();
    oneBarReversal[27].close = 10.0;
    oneBarReversal[28].close = 9.84;
    TradingSignal oneBarBlocked = algorithm->evaluate(
        quote, daily, oneBarReversal, &sufficientHolding);
    assert(oneBarBlocked.type == SignalType::None);

    // 当日成交已经闭合却残留开仓价时停止发信号，避免状态漂移后重复下单。
    Holding driftedHolding = sufficientHolding;
    driftedHolding.todayTradeDate = currentDateToken();
    driftedHolding.todayBuyQuantity = 6000;
    driftedHolding.todayBuyPrice = 9.90;
    driftedHolding.todaySellQuantity = 6000;
    driftedHolding.todaySellPrice = 10.00;
    driftedHolding.todayT0OpenPrice = 9.90;
    TradingSignal driftBlocked = algorithm->evaluate(quote, daily, minute,
                                                      &driftedHolding);
    assert(driftBlocked.type == SignalType::None);
    assert(driftBlocked.message.find("持仓状态校验失败") != std::string::npos);

    // 当前数量无法由日初基线和已登记成交解释时，视为外部持仓变化并暂停策略。
    Holding externallyChanged = sufficientHolding;
    externallyChanged.tradeBaseDate = currentDateToken();
    externallyChanged.tradeBaseQuantity = sufficientHolding.quantity;
    externallyChanged.tradeBaseCost = sufficientHolding.cost;
    externallyChanged.quantity -= 100;
    TradingSignal externalChangeBlocked = algorithm->evaluate(
        quote, daily, minute, &externallyChanged);
    assert(externalChangeBlocked.type == SignalType::None);
    assert(externalChangeBlocked.message.find("外部持仓变动") != std::string::npos);

    // 涨跌停附近不再开新腿；已有腿则优先发出等量应急闭合信号。
    Quote upperLimitQuote = quote;
    upperLimitQuote.previousClose = 10.0;
    upperLimitQuote.price = 11.0;
    auto upperLimitMinute = makeFlatMinuteLinesEndingAt(10, 30, 11.0);
    TradingSignal upperLimitBlocked = algorithm->evaluate(
        upperLimitQuote, daily, upperLimitMinute, &sufficientHolding);
    assert(upperLimitBlocked.type == SignalType::None);
    assert(upperLimitBlocked.message.find("涨停") != std::string::npos);

    Holding pendingSell = sufficientHolding;
    pendingSell.quantity = 36000;
    pendingSell.todayTradeDate = currentDateToken();
    pendingSell.todayBuyQuantity = 6000;
    pendingSell.todayBuyPrice = 10.0;
    pendingSell.todayT0OpenPrice = 10.0;
    Quote lowerLimitQuote = quote;
    lowerLimitQuote.previousClose = 10.0;
    lowerLimitQuote.price = 9.0;
    auto lowerLimitMinute = makeFlatMinuteLinesEndingAt(10, 30, 9.0);
    TradingSignal emergencySell = algorithm->evaluate(
        lowerLimitQuote, daily, lowerLimitMinute, &pendingSell);
    assert(emergencySell.type == SignalType::Sell);
    assert(emergencySell.suggestedQuantity == 6000);
    assert(emergencySell.message.find("跌停应急") != std::string::npos);
    assert(emergencySell.windowLow >= 9.0);

    // 13:55 仍可正常开仓；14:01 起进入只平不开阶段，并为 14:40 闭合预留时间。
    auto at1355 = makeBuyReversalMinuteLines();
    retimeMinuteLines(at1355, 13, 55);
    TradingSignal beforeEntryCutoff = algorithm->evaluate(quote, daily, at1355,
                                                           &sufficientHolding);
    assert(beforeEntryCutoff.type == SignalType::Buy);
    auto at1401 = makeBuyReversalMinuteLines();
    retimeMinuteLines(at1401, 14, 1);
    TradingSignal afterEntryCutoff = algorithm->evaluate(quote, daily, at1401,
                                                         &sufficientHolding);
    assert(afterEntryCutoff.type == SignalType::None);
    assert(afterEntryCutoff.message.find("14:01") != std::string::npos);
    assert(afterEntryCutoff.message.find("14:40") != std::string::npos);

    // 持续单边上涨时应切换为趋势模式，顺势买入而不是等待均值回落。
    auto risingMinute = makeTrendMinuteLines(true, 10.00, 0.01);
    Quote risingQuote = quote;
    risingQuote.price = risingMinute.back().close;
    TradingSignal trendBuy = algorithm->evaluate(risingQuote, daily, risingMinute,
                                                  &sufficientHolding);
    assert(trendBuy.type == SignalType::Buy);
    assert(trendBuy.message.find("单边上涨") != std::string::npos);
    assert(trendBuy.expectedGrossProfit >= trendBuy.estimatedCost * 2.0);
    double meanWindowRate = (buySignal.windowHigh - buySignal.windowLow) /
                            buySignal.currentPrice;
    double trendWindowRate = (trendBuy.windowHigh - trendBuy.windowLow) /
                             trendBuy.currentPrice;
    assert(trendWindowRate > meanWindowRate);
    assert(trendWindowRate <=
           defaultT0StrategyParameters().maximumLimitWindowRate * 2.0 + 0.000001);

    // 累计上涨但来回跳动的序列只有在降低拟合度门槛后才可被识别为趋势。
    auto noisyUpward = makeNoisyUpwardMinuteLines();
    Quote noisyQuote = quote;
    noisyQuote.price = noisyUpward.back().close;
    T0StrategyParameters strictRegression = defaultT0StrategyParameters();
    strictRegression.minimumTrendThreshold = 0.0;
    strictRegression.trendVolatilityMultiplier = 0.0;
    strictRegression.trendDirectionRatio = 0.0;
    strictRegression.shortMomentumThreshold = 0.0;
    strictRegression.minimumRegressionSlopePerMinute = 0.0;
    strictRegression.regressionSlopeVolatilityMultiplier = 0.0;
    strictRegression.minimumRegressionFit = 0.95;
    auto strictAlgorithm = createT0TradingAlgorithm("严格回归", strictRegression);
    TradingSignal noisyBlocked = strictAlgorithm->evaluate(noisyQuote, daily, noisyUpward,
                                                            &sufficientHolding);
    assert(noisyBlocked.type == SignalType::None);
    T0StrategyParameters looseRegression = strictRegression;
    looseRegression.minimumRegressionFit = 0.0;
    auto looseAlgorithm = createT0TradingAlgorithm("宽松回归", looseRegression);
    TradingSignal noisyAccepted = looseAlgorithm->evaluate(noisyQuote, daily, noisyUpward,
                                                            &sufficientHolding);
    assert(noisyAccepted.type == SignalType::Buy);
    assert(noisyAccepted.message.find("线性回归确认") != std::string::npos);

    // 长周期仍上涨但最近短窗已经走平时，滞后过滤器应阻止追入。
    auto staleUptrend = makeStaleUptrendMinuteLines();
    Quote staleTrendQuote = quote;
    staleTrendQuote.price = staleUptrend.back().close;
    T0StrategyParameters lagFiltered = defaultT0StrategyParameters();
    lagFiltered.trendVolatilityMultiplier = 0.0;
    lagFiltered.shortMomentumThreshold = 0.0;
    lagFiltered.minimumRegressionSlopePerMinute = 0.0;
    lagFiltered.regressionSlopeVolatilityMultiplier = 0.0;
    lagFiltered.minimumRecentToLongSlopeRatio = 0.45;
    auto lagFilteredAlgorithm = createT0TradingAlgorithm("滞后过滤", lagFiltered);
    TradingSignal staleTrendBlocked = lagFilteredAlgorithm->evaluate(
        staleTrendQuote, daily, staleUptrend, &sufficientHolding);
    assert(staleTrendBlocked.type == SignalType::None);
    T0StrategyParameters lagDisabled = lagFiltered;
    lagDisabled.minimumRecentToLongSlopeRatio = 0.0;
    auto lagDisabledAlgorithm = createT0TradingAlgorithm("关闭滞后过滤", lagDisabled);
    TradingSignal staleTrendAccepted = lagDisabledAlgorithm->evaluate(
        staleTrendQuote, daily, staleUptrend, &sufficientHolding);
    assert(staleTrendAccepted.type == SignalType::Buy);

    // 持续单边下跌时应先卖出底仓，随后等待更低价格等量买回。
    auto fallingMinute = makeTrendMinuteLines(false, 10.00, 0.01);
    Quote fallingQuote = quote;
    fallingQuote.price = fallingMinute.back().close;
    TradingSignal trendSell = algorithm->evaluate(fallingQuote, daily, fallingMinute,
                                                   &sufficientHolding);
    assert(trendSell.type == SignalType::Sell);
    assert(trendSell.message.find("单边下跌") != std::string::npos);
    assert(trendSell.expectedGrossProfit >= trendSell.estimatedCost * 2.0);

    // 高波动趋势中的 0.40% 浮盈仍低于两个动态目标时继续持有。
    Holding outstandingTrendBuy = sufficientHolding;
    outstandingTrendBuy.quantity = 36000;
    outstandingTrendBuy.todayTradeDate = currentDateToken();
    outstandingTrendBuy.todayBuyQuantity = 6000;
    outstandingTrendBuy.todayBuyPrice = 10.00;
    auto continuedRise = makeTrendMinuteLines(true, 9.75, 0.01);
    Quote quickProfitQuote = quote;
    quickProfitQuote.price = continuedRise.back().close;
    TradingSignal keepTrendPosition = algorithm->evaluate(quickProfitQuote, daily,
                                                          continuedRise,
                                                          &outstandingTrendBuy);
    assert(keepTrendPosition.type == SignalType::None);

    // 当前未闭合腿使用独立开仓价；达到高波动动态目标后再快速止盈。
    outstandingTrendBuy.todayT0OpenPrice = 9.85;
    TradingSignal quickTakeProfit = algorithm->evaluate(quickProfitQuote, daily, continuedRise,
                                                        &outstandingTrendBuy);
    assert(quickTakeProfit.type == SignalType::Sell);
    assert(quickTakeProfit.message.find("快速卖出止盈") != std::string::npos);
    assert(quickTakeProfit.expectedNetProfit > 0.0);

    Holding outstandingTrendSell = sufficientHolding;
    outstandingTrendSell.quantity = 24000;
    outstandingTrendSell.todayTradeDate = currentDateToken();
    outstandingTrendSell.todaySellQuantity = 6000;
    outstandingTrendSell.todaySellPrice = 10.00;
    outstandingTrendSell.todayT0OpenPrice = 10.15;
    auto continuedFall = makeTrendMinuteLines(false, 10.25, 0.01);
    Quote quickBuybackQuote = quote;
    quickBuybackQuote.price = continuedFall.back().close;
    TradingSignal quickBuyback = algorithm->evaluate(quickBuybackQuote, daily,
                                                     continuedFall,
                                                     &outstandingTrendSell);
    assert(quickBuyback.type == SignalType::Buy);
    assert(quickBuyback.message.find("快速买回止盈") != std::string::npos);
    assert(quickBuyback.expectedNetProfit > 0.0);

    // 买入后转为明显下跌趋势时应等量止损，避免未闭合敞口继续扩大。
    Holding losingTrendBuy = outstandingTrendBuy;
    losingTrendBuy.todayT0OpenPrice = 10.00;
    auto reversedDown = makeTrendMinuteLines(false, 10.24, 0.01);
    Quote stopLossQuote = quote;
    stopLossQuote.price = reversedDown.back().close;
    TradingSignal stopLoss = algorithm->evaluate(stopLossQuote, daily, reversedDown,
                                                 &losingTrendBuy);
    assert(stopLoss.type == SignalType::Sell);
    assert(stopLoss.message.find("止损") != std::string::npos);

    // 完成一轮后允许再次开仓，但必须相对上一轮平仓价继续移动至少 0.50%。
    Holding oneCompletedRound = sufficientHolding;
    oneCompletedRound.todayTradeDate = currentDateToken();
    oneCompletedRound.todayBuyQuantity = 6000;
    oneCompletedRound.todayBuyPrice = 10.00;
    oneCompletedRound.todaySellQuantity = 6000;
    oneCompletedRound.todaySellPrice = 10.10;
    oneCompletedRound.todayT0CompletedRounds = 1;
    oneCompletedRound.todayT0LastClosePrice = 10.10;
    TradingSignal secondRound = algorithm->evaluate(risingQuote, daily, risingMinute,
                                                    &oneCompletedRound);
    assert(secondRound.type == SignalType::Buy);
    assert(secondRound.message.find("第 2/3 轮") != std::string::npos);

    Holding waitingForReentry = oneCompletedRound;
    waitingForReentry.todayT0LastClosePrice = risingQuote.price * 0.999;
    TradingSignal reentryBlocked = algorithm->evaluate(risingQuote, daily, risingMinute,
                                                       &waitingForReentry);
    assert(reentryBlocked.type == SignalType::None);
    assert(reentryBlocked.message.find("0.50%") != std::string::npos);

    Holding dailyLimitReached = oneCompletedRound;
    dailyLimitReached.todayBuyQuantity = 18000;
    dailyLimitReached.todaySellQuantity = 18000;
    dailyLimitReached.todayT0CompletedRounds = 3;
    TradingSignal fourthRoundBlocked = algorithm->evaluate(risingQuote, daily, risingMinute,
                                                           &dailyLimitReached);
    assert(fourthRoundBlocked.type == SignalType::None);
    assert(fourthRoundBlocked.message.find("3 轮") != std::string::npos);

    // 高位先卖后若一直未满足盈利回补条件，14:40 必须等量买回，不能继续卖出。
    Holding outstandingSell;
    outstandingSell.symbol = "sh600000";
    outstandingSell.quantity = 24000;
    outstandingSell.cost = 10.0;
    outstandingSell.todayTradeDate = currentDateToken();
    outstandingSell.todaySellQuantity = 6000;
    outstandingSell.todaySellPrice = 10.10;
    Quote closingQuote = quote;
    closingQuote.price = 10.10;
    auto beforeClosingMinute = makeFlatMinuteLinesEndingAt(14, 39, closingQuote.price);
    TradingSignal notForcedEarly = algorithm->evaluate(closingQuote, daily,
                                                       beforeClosingMinute,
                                                       &outstandingSell);
    assert(notForcedEarly.type == SignalType::None);
    auto closingMinute = makeFlatMinuteLinesEndingAt(14, 40, closingQuote.price);
    TradingSignal forcedBuy = algorithm->evaluate(closingQuote, daily, closingMinute,
                                                   &outstandingSell);
    assert(forcedBuy.type == SignalType::Buy);
    assert(forcedBuy.suggestedQuantity == 6000);

    outstandingSell.quantity += 6000;
    outstandingSell.todayBuyQuantity = 6000;
    outstandingSell.todayBuyPrice = 10.20;
    TradingSignal completed = algorithm->evaluate(closingQuote, daily, closingMinute,
                                                   &outstandingSell);
    assert(completed.type == SignalType::None);

    std::cout << "T0 strategy tests passed\n";
    return 0;
}
