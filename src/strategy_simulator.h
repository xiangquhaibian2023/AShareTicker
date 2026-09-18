#pragma once

#include "domain.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ashare {

// 模拟成交费用模板。费率使用小数，例如 0.00025 表示万分之 2.5。
struct SimulationFeeSchedule {
    std::string id = "program_default";
    std::string name = "程序默认（万 1）";
    double commissionRate = 0.0001;
    double minimumCommission = 5.0;
    double stockStampDutyRate = 0.0005;
    double stockTransferFeeRate = 0.00001;
    double slippageRatePerSide = 0.0002;
};

struct SimulationParameters {
    double initialCashPerSymbol = 100000.0;
    SimulationFeeSchedule fees;
    // 未单独配置的标的使用 fees；命中证券代码时使用独立模板。
    std::map<std::string, SimulationFeeSchedule> feesBySymbol;
};

// 东方财富证券基础账户公开默认佣金：股票和场内基金万分之 2.5，单笔最低 5 元。
SimulationFeeSchedule eastmoneyBasicFeeSchedule();

// 一笔由回放信号触发的模拟成交。价格已经包含对交易者不利的滑点。
struct SimulatedTrade {
    std::string strategyId;
    std::string algorithm;
    std::string symbol;
    SignalType side = SignalType::None;
    long long quantity = 0;
    double signalPrice = 0.0;
    double executionPrice = 0.0;
    double fees = 0.0;
    std::string time;
};

// 单个算法、单个标的在当前回放分钟的收益快照。
struct SimulationSnapshot {
    std::string strategyId;
    std::string symbol;
    long long quantity = 0;
    double cost = 0.0;
    double cash = 0.0;
    double marketValue = 0.0;
    double equity = 0.0;
    double initialEquity = 0.0;
    double openingEquity = 0.0;
    double dailyProfit = 0.0;
    double totalProfit = 0.0;
    double totalReturnPercent = 0.0;
    double realizedProfit = 0.0;
    double t0Profit = 0.0;
    double dailyFees = 0.0;
    double totalFees = 0.0;
    int dailyTradeCount = 0;
    int tradeCount = 0;
};

// 某算法在全部观察标的上的合计收益。
struct SimulationSummary {
    double equity = 0.0;
    double dailyProfit = 0.0;
    double totalProfit = 0.0;
    double totalReturnPercent = 0.0;
    double t0Profit = 0.0;
    double totalFees = 0.0;
    long long quantity = 0;
    int tradeCount = 0;
};

struct SimulationExecution {
    bool signalTransition = false;
    bool executed = false;
    std::string message;
    std::optional<SimulatedTrade> trade;
};

// 回放模拟账户。账户只存在于内存，加载新日期或重置回放时清空。
class StrategySimulator {
public:
    static constexpr double initialCashPerSymbol = 100000.0;

    explicit StrategySimulator(SimulationParameters parameters = {});

    void reset();
    void ensureAccount(const StrategyConfig& strategy, const Quote& quote,
                       const Holding* initialHolding);
    Holding holdingContext(const std::string& strategyId, const std::string& symbol) const;
    SimulationExecution processSignal(const StrategyConfig& strategy,
                                      const TradingSignal& signal,
                                      const Quote& quote,
                                      const Holding* initialHolding);

    std::optional<SimulationSnapshot> snapshot(const std::string& strategyId,
                                               const std::string& symbol,
                                               double currentPrice) const;
    SimulationSummary summary(const std::string& strategyId,
                              const std::map<std::string, Quote>& quotes) const;
    const std::vector<SimulatedTrade>& trades() const;

private:
    struct Account {
        std::string strategyId;
        std::string algorithm;
        std::string symbol;
        StrategyKind kind = StrategyKind::MovingAverageMomentum;
        std::string tradingDate;
        double cash = 0.0;
        double initialEquity = 0.0;
        double openingEquity = 0.0;
        long long openingQuantity = 0;
        long long quantity = 0;
        long long todayBuyQuantity = 0;
        long long todaySellQuantity = 0;
        double todayBuyValue = 0.0;
        double todaySellValue = 0.0;
        int t0CompletedRounds = 0;
        double t0OpenPrice = 0.0;
        double t0LastClosePrice = 0.0;
        double cost = 0.0;
        double realizedProfit = 0.0;
        double t0RoundCashFlow = 0.0;
        double t0Profit = 0.0;
        double dailyFees = 0.0;
        double totalFees = 0.0;
        int dailyTradeCount = 0;
        int tradeCount = 0;
        SignalType lastSignal = SignalType::None;
    };

    static std::string accountKey(const std::string& strategyId, const std::string& symbol);
    static long long roundToLot(long long quantity);
    Account& account(const StrategyConfig& strategy, const Quote& quote,
                     const Holding* initialHolding);
    void beginTradingDay(Account& account, const Quote& quote);
    const SimulationFeeSchedule& feeSchedule(const std::string& symbol) const;

    SimulationParameters parameters_;
    std::map<std::string, Account> accounts_;
    std::vector<SimulatedTrade> trades_;
};

}  // namespace ashare
