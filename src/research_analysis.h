#pragma once

#include "domain.h"
#include "strategy_simulator.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ashare {

// 单个标的在某个信号日的后续验证结果。后续收益按信号方向折算：
// 买入信号期待上涨，卖出信号期待下跌。
struct ResearchSecurityResult {
    std::string signalDate;
    std::string followupDate;
    std::string symbol;
    std::string name;
    std::string algorithm;
    SignalType direction = SignalType::None;
    double signalDayChangePercent = 0.0;
    double basePrice = 0.0;
    double followupHigh = 0.0;
    double followupClose = 0.0;
    double comparisonPrice = 0.0;
    double followupReturnPercent = 0.0;
    double benchmarkReturnPercent = 0.0;
    double excessReturnPercent = 0.0;
    double simulationReturnPercent = 0.0;
    double simulationProfit = 0.0;
    double t0Profit = 0.0;
    double fees = 0.0;
    int signalCount = 0;
    int tradeCount = 0;
};

// 单个标的在一个交易日结束后的连续账户快照。与信号明细分开保存，
// 即使当天没有新信号，也能正确计算持仓市值变化和跨日累计盈亏。
struct ResearchAccountDayResult {
    std::string date;
    std::string symbol;
    std::string name;
    long long quantity = 0;
    double cost = 0.0;
    double cash = 0.0;
    double currentPrice = 0.0;
    double marketValue = 0.0;
    double equity = 0.0;
    double initialEquity = 0.0;
    double dailyProfit = 0.0;
    double totalProfit = 0.0;
    double totalReturnPercent = 0.0;
    double t0Profit = 0.0;
    double dailyFees = 0.0;
    int dailyTradeCount = 0;
};

// 研究区间内的一条买卖信号。信号价和信号数量用于还原算法决策，
// 成交字段用于区分实际模拟成交与被资金、持仓或交易制度拒绝的信号。
struct ResearchTradeSignalDetail {
    std::string date;
    std::string time;
    std::string symbol;
    std::string name;
    std::string algorithm;
    SignalType direction = SignalType::None;
    double signalPrice = 0.0;
    long long signalQuantity = 0;
    bool executed = false;
    double executionPrice = 0.0;
    long long executionQuantity = 0;
    double fees = 0.0;
    std::string description;
};

// 一个交易日的等权聚合结果。无信号交易日仍保留，用于绘制连续的基准曲线。
struct ResearchDayResult {
    std::string signalDate;
    std::string followupDate;
    std::vector<ResearchSecurityResult> securities;
    std::vector<ResearchAccountDayResult> accounts;
    std::vector<ResearchTradeSignalDetail> tradeSignals;
    double averageFollowupReturnPercent = 0.0;
    double averageSimulationReturnPercent = 0.0;
    double benchmarkFollowupReturnPercent = 0.0;
    double cumulativeSimulationReturnPercent = 0.0;
    bool hasCumulativeSimulationReturn = false;
    std::map<std::string, TechnicalSwingResult> technicalSwingResults;
};

struct ResearchCurvePoint {
    std::string date;
    double signalCumulativePercent = 0.0;
    double simulationCumulativePercent = 0.0;
    double benchmarkCumulativePercent = 0.0;
};

// 单个标的在研究过程中的非致命错误。保留日期、阶段和原始原因，
// 便于界面直接展示，也方便日志按标的定位数据源问题。
struct ResearchIssue {
    std::string date;
    std::string symbol;
    std::string stage;
    std::string message;
};

// 单个标的在完整研究区间内的连续账户汇总，用于结果筛选和多日盈亏展示。
struct ResearchSymbolSummary {
    std::string symbol;
    std::string name;
    long long openingQuantity = 0;
    double openingCost = 0.0;
    double initialCapital = 0.0;
    long long endingQuantity = 0;
    double endingCost = 0.0;
    double endingCash = 0.0;
    double endingEquity = 0.0;
    double totalProfit = 0.0;
    double totalReturnPercent = 0.0;
    double buyAndHoldProfit = 0.0;
    double strategyIncrementalProfit = 0.0;
    double signalCumulativePercent = 0.0;
    double benchmarkCumulativePercent = 0.0;
    double t0Profit = 0.0;
    double totalFees = 0.0;
    int tradeCount = 0;
    int activeDays = 0;
    std::vector<ResearchCurvePoint> curve;
};

struct AlgorithmResearchReport {
    StrategyConfig strategy;
    std::string startDate;
    std::string endDate;
    std::string benchmarkName = "上证指数";
    int coveredTradingDays = 0;
    int signalDays = 0;
    int securityResultCount = 0;
    double signalCumulativePercent = 0.0;
    double simulationCumulativePercent = 0.0;
    double simulationTotalProfit = 0.0;
    double simulationInitialCapital = 0.0;
    double buyAndHoldProfit = 0.0;
    double strategyIncrementalProfit = 0.0;
    double benchmarkCumulativePercent = 0.0;
    double signalExcessPercent = 0.0;
    double simulationExcessPercent = 0.0;
    std::vector<ResearchDayResult> days;
    std::vector<ResearchCurvePoint> curve;
    std::map<std::string, ResearchSymbolSummary> symbolSummaries;
    std::vector<ResearchIssue> issues;
};

struct AlgorithmResearchRequest {
    StrategyConfig strategy;
    std::map<std::string, std::string> symbols;
    std::map<std::string, Holding> openingHoldings;
    double initialCash = 100000.0;
    SimulationFeeSchedule fees = eastmoneyBasicFeeSchedule();
    std::map<std::string, SimulationFeeSchedule> feesBySymbol;
    std::string startDate;
    std::string endDate;
    std::filesystem::path replayCacheDirectory;
    std::string benchmarkSymbol = "sh000001";
    std::string benchmarkName = "上证指数";
};

using ResearchProgressCallback =
    std::function<void(int completed, int total, const std::string& message)>;
using ResearchCancelCallback = std::function<bool()>;

// 根据逐日明细计算累计曲线和汇总卡片。该函数不访问网络，便于单元测试。
AlgorithmResearchReport buildAlgorithmResearchReport(
    const StrategyConfig& strategy, const std::string& startDate,
    const std::string& endDate, const std::string& benchmarkName,
    std::vector<ResearchDayResult> days,
    const std::vector<KLine>& benchmarkDailyLines);

// 自动加载真实历史行情并完整重放每个交易日，无需用户逐分钟操作。
AlgorithmResearchReport runAlgorithmResearch(
    const AlgorithmResearchRequest& request,
    const ResearchProgressCallback& progress = {},
    const ResearchCancelCallback& cancelled = {});

}  // namespace ashare
