#include "storage.h"
#include "utils.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

int main() {
    using namespace ashare;

    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 "ashare_holding_store_tests.tsv";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    Holding holding;
    holding.symbol = "sh600000";
    holding.quantity = 30000;
    holding.cost = 9.876;
    holding.todayTradeDate = currentDateToken();
    holding.todayBuyQuantity = 12000;
    holding.todayBuyPrice = 9.950;
    holding.todaySellQuantity = 12000;
    holding.todaySellPrice = 10.080;
    holding.todayFees = 42.50;
    holding.todayT0CompletedRounds = 2;
    holding.todayT0OpenPrice = 0.0;
    holding.todayT0LastClosePrice = 10.120;
    holding.tradeBaseDate = currentDateToken();
    holding.tradeBaseQuantity = 30000;
    holding.tradeBaseCost = 9.876;

    HoldingStore store(path);
    store.save({{holding.symbol, holding}});
    std::map<std::string, Holding> loaded = store.load();
    assert(loaded.size() == 1);
    const Holding& restored = loaded.at(holding.symbol);
    assert(restored.todayT0CompletedRounds == 2);
    assert(restored.todayT0OpenPrice == 0.0);
    assert(restored.todayT0LastClosePrice == 10.120);
    assert(restored.tradeBaseDate == currentDateToken());
    assert(restored.tradeBaseQuantity == 30000);
    assert(std::abs(restored.tradeBaseCost - 9.876) < 1e-9);
    assert(std::abs(closedT0NetProfit(restored, currentDateToken()) - 1517.50) < 0.01);

    // 旧版 9 列持仓文件继续可读，新字段使用默认值。
    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "sh600000\t1000\t10.0\t" << currentDateToken()
               << "\t0\t0\t0\t0\t0\n";
    }
    loaded = store.load();
    assert(loaded.size() == 1);
    assert(loaded.at("sh600000").todayT0CompletedRounds == 0);
    assert(loaded.at("sh600000").todayT0OpenPrice == 0.0);

    std::filesystem::path signalPath = std::filesystem::temp_directory_path() /
                                       "ashare_strategy_signal_store_tests.tsv";
    std::filesystem::remove(signalPath, ignored);
    StrategySignalRecord record;
    record.dataMode = MarketDataMode::Live;
    record.strategyId = "builtin_t0_intraday";
    record.signal.symbol = "sh513050";
    record.signal.name = "中概互联网ETF";
    record.signal.algorithm = "T0 日内回转";
    record.signal.type = SignalType::Buy;
    record.signal.currentPrice = 1.234;
    record.signal.windowLow = 1.230;
    record.signal.windowHigh = 1.238;
    record.signal.message = "测试\t信号\n明细";
    record.signal.time = "2026-08-11 10:30:05";
    record.signal.suggestedQuantity = 1000;
    record.signal.estimatedCost = 10.0;
    record.signal.expectedGrossProfit = 25.0;
    record.signal.expectedNetProfit = 15.0;
    StrategySignalStore signalStore(signalPath);
    signalStore.save("20260811", {record});
    auto restoredSignals = signalStore.load("20260811");
    assert(restoredSignals.size() == 1);
    assert(restoredSignals.front().strategyId == record.strategyId);
    assert(restoredSignals.front().signal.symbol == record.signal.symbol);
    assert(restoredSignals.front().signal.suggestedQuantity == 1000);
    assert(restoredSignals.front().signal.message == "测试 信号 明细");
    assert(signalStore.load("20260812").empty());

    std::filesystem::path tradePath = std::filesystem::temp_directory_path() /
                                      "ashare_trade_store_tests.tsv";
    std::filesystem::remove(tradePath, ignored);
    TradeRecord trade;
    trade.id = "manual-1";
    trade.symbol = "sh513050";
    trade.time = "2026-08-11 10:05";
    trade.side = SignalType::Buy;
    trade.quantity = 1000;
    trade.price = 1.234;
    trade.commission = 5.0;
    TradeRecordStore tradeStore(tradePath);
    tradeStore.save({trade});
    auto restoredTrades = tradeStore.load();
    assert(restoredTrades.size() == 1);
    assert(restoredTrades.front().id == trade.id);
    assert(restoredTrades.front().quantity == 1000);
    assert(std::abs(restoredTrades.front().price - 1.234) < 1e-9);

    std::filesystem::path fundsPath = std::filesystem::temp_directory_path() /
                                      "ashare_account_funds_tests.tsv";
    std::filesystem::remove(fundsPath, ignored);
    AccountFundsStore fundsStore(fundsPath);
    fundsStore.save({500000.0, 120000.0, "2026-08-11 15:00"});
    AccountFunds funds = fundsStore.load();
    assert(std::abs(funds.totalAssets - 500000.0) < 1e-9);
    assert(std::abs(funds.availableCash - 120000.0) < 1e-9);
    assert(funds.updatedAt == "2026-08-11 15:00");

    std::filesystem::remove(path, ignored);
    std::filesystem::remove(signalPath, ignored);
    std::filesystem::remove(tradePath, ignored);
    std::filesystem::remove(fundsPath, ignored);
    std::cout << "Holding storage tests passed\n";
    return 0;
}
