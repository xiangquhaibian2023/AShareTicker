#pragma once

#include "domain.h"
#include "strategy_simulator.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ashare {

class ReplayEngine;
class TradingAlgorithm;

// 一次买卖信号及其模拟执行结果。保留未成交信号，便于研究页面说明
// 资金、持仓或交易制度导致的拒绝原因。
struct StrategySignalExecution {
    TradingSignal signal;
    bool executed = false;
    std::optional<SimulatedTrade> trade;
    std::string executionMessage;
};

// 单日策略回放结果。客户端用信号序列恢复当日记录，回测工具用模拟账户统计收益。
struct StrategyDayBacktestResult {
    std::map<std::string, TechnicalSwingResult> technicalSwingResults;
    std::vector<TradingSignal> signalTransitions;
    std::vector<StrategySignalExecution> signalExecutions;
    std::map<std::string, Quote> latestQuotes;
    StrategySimulator simulator;
};

// 由外部持有模拟账户的单日结果。算法研究使用该接口让现金、持仓和成本跨日延续。
struct StrategyDayReplayResult {
    std::map<std::string, TechnicalSwingResult> technicalSwingResults;
    std::vector<TradingSignal> signalTransitions;
    std::vector<StrategySignalExecution> signalExecutions;
    std::map<std::string, Quote> latestQuotes;
};

// 按交易分钟重放已加载到 ReplayEngine 的真实行情，并自动模拟信号对应的委托成交。
StrategyDayBacktestResult runStrategyDayBacktest(
    const StrategyConfig& strategy, ReplayEngine& replay,
    const std::set<std::string>& symbols,
    const std::map<std::string, Holding>& openingHoldings,
    const TradingAlgorithm* algorithmOverride = nullptr);

StrategyDayReplayResult runStrategyDayWithSimulator(
    const StrategyConfig& strategy, ReplayEngine& replay,
    const std::set<std::string>& symbols,
    const std::map<std::string, Holding>& openingHoldings,
    StrategySimulator& simulator,
    const TradingAlgorithm* algorithmOverride = nullptr,
    const std::map<std::string, SimulationFeeSchedule>* feesBySymbol = nullptr);

// 从当前持仓还原日初底仓，避免重启时把当日已成交数量重复带入回放。
std::map<std::string, Holding> makeOpeningHoldings(
    const std::map<std::string, Holding>& currentHoldings,
    const std::string& replayDate);

}  // namespace ashare
