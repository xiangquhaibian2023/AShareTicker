#pragma once

#include "domain.h"

#include <string>
#include <vector>

namespace ashare {

struct TradeLedgerSnapshot {
    bool valid = true;
    std::string error;
    long long openingQuantity = 0;
    double openingCost = 0.0;
    long long quantity = 0;
    double cost = 0.0;
    double realizedProfit = 0.0;
    double floatingProfit = 0.0;
    double totalProfit = 0.0;
    double dailyProfit = 0.0;
    double closedT0Profit = 0.0;
    double totalFees = 0.0;
    long long buyQuantity = 0;
    long long sellQuantity = 0;
    double buyValue = 0.0;
    double sellValue = 0.0;
    int tradeCount = 0;
};

std::string tradeDateToken(const std::string& dateTime);
std::string generateTradeRecordId();
std::vector<TradeRecord> tradesFor(const std::vector<TradeRecord>& records,
                                   const std::string& symbol,
                                   const std::string& dateToken,
                                   const std::string& strategyId = {});
// 同一策略、标的和日期录入手工成交后，以手工账本覆盖自动模拟成交。
std::vector<TradeRecord> effectiveStrategyTrades(const std::vector<TradeRecord>& records,
                                                 const std::string& strategyId,
                                                 const std::string& symbol,
                                                 const std::string& dateToken);
TradeLedgerSnapshot calculateTradeLedger(long long openingQuantity, double openingCost,
                                         const std::vector<TradeRecord>& trades,
                                         double currentPrice = 0.0,
                                         double previousClose = 0.0);
void ensureHoldingTradeBase(Holding& holding, const std::string& dateToken);
bool recalculateHoldingFromTrades(Holding& holding,
                                  const std::vector<TradeRecord>& allTrades,
                                  const std::string& dateToken,
                                  std::string* error = nullptr);

}  // namespace ashare
