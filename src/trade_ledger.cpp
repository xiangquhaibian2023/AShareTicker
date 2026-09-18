#include "trade_ledger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ashare {

namespace {

bool validTrade(const TradeRecord& trade) {
    return !trade.id.empty() && !trade.symbol.empty() &&
           (trade.side == SignalType::Buy || trade.side == SignalType::Sell) &&
           trade.quantity > 0 && std::isfinite(trade.price) && trade.price > 0.0 &&
           std::isfinite(trade.commission) && trade.commission >= 0.0 &&
           std::isfinite(trade.stampDuty) && trade.stampDuty >= 0.0 &&
           std::isfinite(trade.transferFee) && trade.transferFee >= 0.0 &&
           std::isfinite(trade.otherFees) && trade.otherFees >= 0.0;
}

}  // namespace

std::string tradeDateToken(const std::string& dateTime) {
    std::string result;
    for (char value : dateTime) {
        if (value >= '0' && value <= '9') {
            result.push_back(value);
            if (result.size() == 8) break;
        }
    }
    return result.size() == 8 ? result : std::string{};
}

std::string generateTradeRecordId() {
    static std::atomic<unsigned long long> sequence{0};
    auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    return std::to_string(ticks) + "-" + std::to_string(sequence.fetch_add(1));
}

std::vector<TradeRecord> tradesFor(const std::vector<TradeRecord>& records,
                                   const std::string& symbol,
                                   const std::string& dateToken,
                                   const std::string& strategyId) {
    std::vector<TradeRecord> result;
    for (const auto& trade : records) {
        if (trade.symbol == symbol && tradeDateToken(trade.time) == dateToken &&
            (strategyId.empty() || trade.strategyId == strategyId)) {
            result.push_back(trade);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.time != right.time ? left.time < right.time : left.id < right.id;
    });
    return result;
}

std::vector<TradeRecord> effectiveStrategyTrades(const std::vector<TradeRecord>& records,
                                                 const std::string& strategyId,
                                                 const std::string& symbol,
                                                 const std::string& dateToken) {
    auto result = tradesFor(records, symbol, dateToken, strategyId);
    bool hasManual = std::any_of(result.begin(), result.end(), [](const auto& trade) {
        return trade.source == TradeSource::Manual;
    });
    if (hasManual) {
        result.erase(std::remove_if(result.begin(), result.end(), [](const auto& trade) {
                         return trade.source != TradeSource::Manual;
                     }),
                     result.end());
    }
    return result;
}

TradeLedgerSnapshot calculateTradeLedger(long long openingQuantity, double openingCost,
                                         const std::vector<TradeRecord>& inputTrades,
                                         double currentPrice, double previousClose) {
    TradeLedgerSnapshot result;
    result.openingQuantity = openingQuantity;
    result.openingCost = openingCost;
    result.quantity = openingQuantity;
    result.cost = openingQuantity > 0 ? openingCost : 0.0;
    if (openingQuantity < 0 || !std::isfinite(openingCost) || openingCost < 0.0) {
        result.valid = false;
        result.error = "期初持仓数据无效";
        return result;
    }

    std::vector<TradeRecord> trades = inputTrades;
    std::stable_sort(trades.begin(), trades.end(), [](const auto& left, const auto& right) {
        return left.time != right.time ? left.time < right.time : left.id < right.id;
    });
    double buyCostWithFees = 0.0;
    double sellProceedsAfterFees = 0.0;
    for (const auto& trade : trades) {
        if (!validTrade(trade)) {
            result.valid = false;
            result.error = "成交记录包含无效字段";
            return result;
        }
        double fees = tradeFees(trade);
        double value = trade.price * static_cast<double>(trade.quantity);
        result.totalFees += fees;
        ++result.tradeCount;
        if (trade.side == SignalType::Buy) {
            double oldCostValue = result.cost * static_cast<double>(result.quantity);
            result.quantity += trade.quantity;
            result.cost = result.quantity > 0
                              ? (oldCostValue + value + fees) /
                                    static_cast<double>(result.quantity)
                              : 0.0;
            result.buyQuantity += trade.quantity;
            result.buyValue += value;
            buyCostWithFees += value + fees;
        } else {
            if (trade.quantity > result.quantity) {
                result.valid = false;
                result.error = "卖出数量超过成交时可用持仓";
                return result;
            }
            result.realizedProfit += (trade.price - result.cost) *
                                         static_cast<double>(trade.quantity) - fees;
            result.quantity -= trade.quantity;
            if (result.quantity == 0) result.cost = 0.0;
            result.sellQuantity += trade.quantity;
            result.sellValue += value;
            sellProceedsAfterFees += value - fees;
        }
    }
    long long matched = std::min(result.buyQuantity, result.sellQuantity);
    if (matched > 0) {
        double averageBuyCost = buyCostWithFees / static_cast<double>(result.buyQuantity);
        double averageSellProceeds = sellProceedsAfterFees /
                                     static_cast<double>(result.sellQuantity);
        result.closedT0Profit = (averageSellProceeds - averageBuyCost) *
                                static_cast<double>(matched);
    }
    if (currentPrice > 0.0 && std::isfinite(currentPrice)) {
        result.floatingProfit = (currentPrice - result.cost) *
                                static_cast<double>(result.quantity);
        result.totalProfit = result.realizedProfit + result.floatingProfit;
        if (previousClose > 0.0 && std::isfinite(previousClose)) {
            double marketValue = currentPrice * static_cast<double>(result.quantity);
            result.dailyProfit = marketValue + result.sellValue - result.buyValue -
                                 result.totalFees - previousClose *
                                 static_cast<double>(openingQuantity);
        }
    }
    return result;
}

void ensureHoldingTradeBase(Holding& holding, const std::string& dateToken) {
    if (holding.tradeBaseDate == dateToken) return;
    holding.tradeBaseDate = dateToken;
    holding.tradeBaseQuantity = holding.quantity;
    holding.tradeBaseCost = holding.cost;
    clearIntradayTrades(holding);
}

bool recalculateHoldingFromTrades(Holding& holding,
                                  const std::vector<TradeRecord>& allTrades,
                                  const std::string& dateToken,
                                  std::string* error) {
    ensureHoldingTradeBase(holding, dateToken);
    auto trades = tradesFor(allTrades, holding.symbol, dateToken);
    TradeLedgerSnapshot snapshot = calculateTradeLedger(
        holding.tradeBaseQuantity, holding.tradeBaseCost, trades);
    if (!snapshot.valid) {
        if (error) *error = snapshot.error;
        return false;
    }
    holding.quantity = snapshot.quantity;
    holding.cost = snapshot.cost;
    holding.todayTradeDate = dateToken;
    holding.todayBuyQuantity = snapshot.buyQuantity;
    holding.todayBuyPrice = snapshot.buyQuantity > 0
                                ? snapshot.buyValue / static_cast<double>(snapshot.buyQuantity)
                                : 0.0;
    holding.todaySellQuantity = snapshot.sellQuantity;
    holding.todaySellPrice = snapshot.sellQuantity > 0
                                 ? snapshot.sellValue / static_cast<double>(snapshot.sellQuantity)
                                 : 0.0;
    holding.todayFees = snapshot.totalFees;
    holding.todayT0CompletedRounds = 0;
    holding.todayT0OpenPrice = 0.0;
    holding.todayT0LastClosePrice = 0.0;
    long long outstanding = 0;
    for (const auto& trade : trades) {
        long long oldOutstanding = outstanding;
        outstanding += trade.side == SignalType::Buy ? trade.quantity : -trade.quantity;
        if (oldOutstanding == 0 && outstanding != 0) {
            holding.todayT0OpenPrice = trade.price;
        } else if (oldOutstanding != 0 && outstanding == 0) {
            ++holding.todayT0CompletedRounds;
            holding.todayT0LastClosePrice = trade.price;
            holding.todayT0OpenPrice = 0.0;
        }
    }
    return true;
}

}  // namespace ashare
