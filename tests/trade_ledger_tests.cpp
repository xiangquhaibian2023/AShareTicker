#include "trade_ledger.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

ashare::TradeRecord trade(const char* id, ashare::SignalType side, long long quantity,
                          double price, double commission,
                          ashare::TradeSource source = ashare::TradeSource::Manual) {
    ashare::TradeRecord result;
    result.id = id;
    result.strategyId = "t0";
    result.symbol = "sh513050";
    result.time = std::string("2026-08-11 10:0") + (side == ashare::SignalType::Buy ? "0" : "5");
    result.side = side;
    result.quantity = quantity;
    result.price = price;
    result.commission = commission;
    result.source = source;
    return result;
}

}  // namespace

int main() {
    using namespace ashare;

    std::vector<TradeRecord> records = {
        trade("buy", SignalType::Buy, 100, 9.8, 5.0),
        trade("sell", SignalType::Sell, 100, 10.2, 5.0),
    };
    TradeLedgerSnapshot snapshot = calculateTradeLedger(1000, 10.0, records, 10.1, 10.0);
    assert(snapshot.valid);
    assert(snapshot.quantity == 1000);
    assert(std::abs(snapshot.cost - 9.98636363636) < 1e-8);
    assert(std::abs(snapshot.realizedProfit - 16.36363636) < 1e-6);
    assert(std::abs(snapshot.floatingProfit - 113.63636364) < 1e-6);
    assert(std::abs(snapshot.dailyProfit - 130.0) < 1e-8);
    assert(std::abs(snapshot.closedT0Profit - 30.0) < 1e-8);
    assert(std::abs(snapshot.totalFees - 10.0) < 1e-8);

    auto excessiveSell = calculateTradeLedger(
        100, 10.0, {trade("bad", SignalType::Sell, 200, 10.1, 5.0)});
    assert(!excessiveSell.valid);

    auto automatic = trade("auto", SignalType::Buy, 100, 9.9, 5.0,
                           TradeSource::StrategySimulation);
    auto manual = trade("manual", SignalType::Sell, 100, 10.2, 5.0);
    auto effective = effectiveStrategyTrades({automatic, manual}, "t0", "sh513050",
                                              "20260811");
    assert(effective.size() == 1);
    assert(effective.front().id == "manual");
    assert(tradeDateToken("2026-08-11 10:05") == "20260811");

    std::cout << "Trade ledger tests passed\n";
    return 0;
}
