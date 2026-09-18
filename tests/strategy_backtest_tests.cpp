#include "replay_engine.h"
#include "strategy_backtest.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace {

std::string minuteTime(int index) {
    int total = index <= 120 ? 9 * 60 + 30 + index : 13 * 60 + index - 121;
    char value[32]{};
    std::snprintf(value, sizeof(value), "2026-08-11 %02d:%02d", total / 60, total % 60);
    return value;
}

}  // namespace

int main() {
    using namespace ashare;

    std::vector<KLine> daily(30);
    for (size_t index = 0; index < daily.size(); ++index) {
        char date[16]{};
        std::snprintf(date, sizeof(date), "2026-07-%02d", static_cast<int>(index + 1));
        daily[index] = KLine{date, 10.0, 10.0, 10.1, 9.9, 1000000.0};
    }

    std::vector<KLine> minute(242);
    for (int index = 0; index < 242; ++index) {
        minute[index] = KLine{minuteTime(index), 10.0, 10.0, 10.0, 10.0, 100000.0};
    }
    for (int index = 0; index < 5; ++index) {
        minute[index] = KLine{minuteTime(index), 9.90, 9.90, 9.90, 9.90, 100000.0};
    }
    // 09:57 触及低点，随后连续两分钟恢复，10:00 回到均值并闭合敞口。
    minute[27] = KLine{minuteTime(27), 9.84, 9.84, 9.84, 9.84, 100000.0};
    minute[28] = KLine{minuteTime(28), 9.86, 9.86, 9.86, 9.86, 100000.0};
    minute[29] = KLine{minuteTime(29), 9.90, 9.90, 9.90, 9.90, 100000.0};

    std::filesystem::path temp = std::filesystem::temp_directory_path() /
                                 "ashare_strategy_backtest_test";
    ReplayEngine replay(temp);
    replay.begin("2026-08-11");
    replay.installSymbolData("sh513050", "测试ETF", std::move(minute), std::move(daily));

    Holding holding;
    holding.symbol = "sh513050";
    holding.quantity = 40000;
    holding.cost = 10.0;
    std::map<std::string, Holding> holdings{{holding.symbol, holding}};
    StrategyConfig strategy{"builtin_t0_intraday", "T0 日内回转", StrategyKind::T0Intraday};
    auto result = runStrategyDayBacktest(strategy, replay, {holding.symbol}, holdings);
    assert(result.signalTransitions.size() == 2);
    assert(result.signalExecutions.size() == 2);
    assert(result.signalTransitions[0].type == SignalType::Buy);
    assert(result.signalTransitions[1].type == SignalType::Sell);
    assert(result.signalExecutions[0].executed && result.signalExecutions[0].trade);
    assert(result.signalExecutions[0].trade->side == SignalType::Buy);
    assert(result.signalExecutions[0].trade->quantity > 0);
    assert(result.signalExecutions[0].trade->executionPrice > 0.0);
    assert(result.signalExecutions[1].executed && result.signalExecutions[1].trade);
    assert(result.signalExecutions[1].trade->side == SignalType::Sell);
    assert(result.simulator.trades().size() == 2);
    auto snapshot = result.simulator.snapshot(strategy.id, holding.symbol, 10.0);
    assert(snapshot && snapshot->t0Profit > 0.0 && snapshot->tradeCount == 2);

    // 日线因子接入同一回测，仅增加可观察结果，不改变成交。
    assert(result.technicalSwingResults.empty());
    strategy.technicalSwing.enableTechnicalSwingStrategy = true;
    auto withFactor = runStrategyDayBacktest(strategy, replay, {holding.symbol}, holdings);
    assert(withFactor.technicalSwingResults.at(holding.symbol).technicalSignal == "INSUFFICIENT_DATA");
    assert(withFactor.signalTransitions.size() == result.signalTransitions.size());
    assert(withFactor.simulator.trades().size() == result.simulator.trades().size());
    auto factorSnapshot = withFactor.simulator.snapshot(strategy.id, holding.symbol, 10.0);
    assert(factorSnapshot && factorSnapshot->t0Profit == snapshot->t0Profit);
    for (size_t i = 0; i < result.signalTransitions.size(); ++i) {
        assert(withFactor.signalTransitions[i].type == result.signalTransitions[i].type);
        assert(withFactor.signalTransitions[i].time == result.signalTransitions[i].time);
    }

    // 研究页为不同标的配置的费率必须同时进入信号成本门槛；高费用模板
    // 会过滤无法覆盖完整成本的同一组行情，而不是只在成交后扣费。
    SimulationFeeSchedule expensiveFees = eastmoneyBasicFeeSchedule();
    expensiveFees.id = "test_expensive";
    expensiveFees.minimumCommission = 1000.0;
    std::map<std::string, SimulationFeeSchedule> feesBySymbol{
        {holding.symbol, expensiveFees}};
    SimulationParameters expensiveParameters;
    expensiveParameters.feesBySymbol = feesBySymbol;
    StrategySimulator expensiveSimulator(expensiveParameters);
    auto filtered = runStrategyDayWithSimulator(
        strategy, replay, {holding.symbol}, holdings, expensiveSimulator,
        nullptr, &feesBySymbol);
    assert(filtered.signalTransitions.empty());
    assert(expensiveSimulator.trades().empty());

    holding.quantity = 40500;
    holding.todayTradeDate = "20260811";
    holding.todayBuyQuantity = 1000;
    holding.todaySellQuantity = 500;
    auto opening = makeOpeningHoldings({{holding.symbol, holding}}, "20260811");
    assert(opening.at(holding.symbol).quantity == 40000);
    assert(opening.at(holding.symbol).todayBuyQuantity == 0);
    assert(opening.at(holding.symbol).todaySellQuantity == 0);

    std::cout << "Strategy backtest tests passed\n";
    return 0;
}
