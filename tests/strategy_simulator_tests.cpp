#include "strategy_simulator.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

bool nearlyEqual(double left, double right, double tolerance = 0.01) {
    return std::fabs(left - right) <= tolerance;
}

ashare::TradingSignal signal(const ashare::Quote& quote, ashare::SignalType type,
                             long long suggested = 0) {
    ashare::TradingSignal result;
    result.symbol = quote.symbol;
    result.name = quote.name;
    result.algorithm = "测试算法";
    result.type = type;
    result.currentPrice = quote.price;
    result.suggestedQuantity = suggested;
    result.time = quote.time;
    return result;
}

}  // namespace

int main() {
    using namespace ashare;

    StrategyConfig momentum{"momentum", "均线动量", StrategyKind::MovingAverageMomentum};
    Quote stock{"sh600000", "浦发银行", 10.0, 10.0, 10.0, 0.0, 0.0, 0.0, 0.0,
                "2026-08-07 10:00"};
    StrategySimulator simulator;

    SimulationExecution buy = simulator.processSignal(momentum, signal(stock, SignalType::Buy),
                                                      stock, nullptr);
    assert(buy.executed && buy.trade && buy.trade->quantity == 1900);
    auto afterBuy = simulator.snapshot(momentum.id, stock.symbol, stock.price);
    assert(afterBuy && afterBuy->quantity == 1900 && afterBuy->cost > stock.price);
    assert(afterBuy->dailyProfit < 0.0 && afterBuy->totalProfit < 0.0);
    // 连续买入信号只产生一次成交。
    assert(!simulator.processSignal(momentum, signal(stock, SignalType::Buy), stock, nullptr).executed);
    simulator.processSignal(momentum, signal(stock, SignalType::None), stock, nullptr);
    SimulationExecution blockedSell = simulator.processSignal(
        momentum, signal(stock, SignalType::Sell), stock, nullptr);
    assert(!blockedSell.executed);  // 普通股票当天新买入不可卖出。

    StrategySimulator seededSimulator;
    Holding seed;
    seed.symbol = stock.symbol;
    seed.quantity = 1000;
    seed.cost = 9.5;
    stock.previousClose = 9.8;
    SimulationExecution sold = seededSimulator.processSignal(
        momentum, signal(stock, SignalType::Sell), stock, &seed);
    assert(sold.executed && sold.trade && sold.trade->quantity == 1000);
    auto afterSell = seededSimulator.snapshot(momentum.id, stock.symbol, stock.price);
    assert(afterSell && afterSell->quantity == 0 && afterSell->tradeCount == 1);
    assert(nearlyEqual(afterSell->totalProfit, 487.90));
    assert(nearlyEqual(afterSell->dailyProfit, 187.90));

    // T0 高位先卖、低位再买时，回补数量优先匹配尚未回转的卖出数量。
    StrategyConfig t0{"t0", "T0 日内回转", StrategyKind::T0Intraday};
    Quote fund{"sh513050", "中概互联网ETF", 1.200, 1.190, 1.195, 0.0, 0.0, 0.0, 0.0,
               "2026-08-07 10:30"};
    Holding fundSeed;
    fundSeed.symbol = fund.symbol;
    fundSeed.quantity = 3000;
    fundSeed.cost = 1.180;
    StrategySimulator t0Simulator;
    assert(t0Simulator.processSignal(t0, signal(fund, SignalType::Sell, 1000),
                                     fund, &fundSeed).executed);
    Holding afterT0Sell = t0Simulator.holdingContext(t0.id, fund.symbol);
    assert(afterT0Sell.todaySellQuantity == 1000 && afterT0Sell.todaySellPrice > 0.0);
    assert(afterT0Sell.todayT0OpenPrice > 0.0 && afterT0Sell.todayT0CompletedRounds == 0);
    t0Simulator.processSignal(t0, signal(fund, SignalType::None), fund, &fundSeed);
    fund.price = 1.180;
    SimulationExecution boughtBack = t0Simulator.processSignal(
        t0, signal(fund, SignalType::Buy, 600), fund, &fundSeed);
    assert(boughtBack.executed && boughtBack.trade->quantity == 1000);
    Holding afterT0Close = t0Simulator.holdingContext(t0.id, fund.symbol);
    assert(afterT0Close.todayBuyQuantity == 1000 && afterT0Close.todayBuyPrice > 0.0);
    assert(afterT0Close.todayT0CompletedRounds == 1);
    assert(afterT0Close.todayT0OpenPrice == 0.0 && afterT0Close.todayT0LastClosePrice > 0.0);
    auto t0Result = t0Simulator.snapshot(t0.id, fund.symbol, fund.price);
    assert(t0Result && t0Result->quantity == 3000 && t0Result->tradeCount == 2);
    assert(t0Result->t0Profit > 9.0 && t0Result->t0Profit < 10.0);

    // 第一轮闭合后仍可开始第二轮，且新未闭合腿记录独立开仓价。
    t0Simulator.processSignal(t0, signal(fund, SignalType::None), fund, &fundSeed);
    fund.price = 1.210;
    SimulationExecution secondSell = t0Simulator.processSignal(
        t0, signal(fund, SignalType::Sell, 1000), fund, &fundSeed);
    assert(secondSell.executed && secondSell.trade->quantity == 1000);
    Holding secondOpen = t0Simulator.holdingContext(t0.id, fund.symbol);
    assert(secondOpen.todayT0CompletedRounds == 1 && secondOpen.todayT0OpenPrice > 0.0);
    t0Simulator.processSignal(t0, signal(fund, SignalType::None), fund, &fundSeed);
    fund.price = 1.190;
    assert(t0Simulator.processSignal(t0, signal(fund, SignalType::Buy, 1000),
                                     fund, &fundSeed).executed);
    Holding secondClose = t0Simulator.holdingContext(t0.id, fund.symbol);
    assert(secondClose.todayT0CompletedRounds == 2 && secondClose.todayT0OpenPrice == 0.0);
    auto secondResult = t0Simulator.snapshot(t0.id, fund.symbol, fund.price);
    assert(secondResult && secondResult->t0Profit > 18.0 && secondResult->t0Profit < 20.0);

    // 连续回测跨日时保留现金、持仓和成本，只重置当日盈亏基准。
    SimulationParameters continuousParameters;
    continuousParameters.initialCashPerSymbol = 10000.0;
    StrategySimulator continuous(continuousParameters);
    stock.price = 10.0;
    stock.previousClose = 9.8;
    stock.time = "2026-08-07 15:00";
    continuous.ensureAccount(momentum, stock, &seed);
    auto firstDay = continuous.snapshot(momentum.id, stock.symbol, stock.price);
    assert(firstDay && nearlyEqual(firstDay->totalProfit, 500.0));
    stock.price = 11.0;
    stock.previousClose = 10.0;
    stock.time = "2026-08-08 09:30";
    continuous.ensureAccount(momentum, stock, &seed);
    auto secondDay = continuous.snapshot(momentum.id, stock.symbol, stock.price);
    assert(secondDay && secondDay->quantity == 1000 && nearlyEqual(secondDay->cost, 9.5));
    assert(nearlyEqual(secondDay->dailyProfit, 1000.0));
    assert(nearlyEqual(secondDay->totalProfit, 1500.0));

    // 算法研究默认佣金模板使用东方财富基础账户的万分之 2.5。
    SimulationParameters eastmoneyParameters;
    eastmoneyParameters.initialCashPerSymbol = 200000.0;
    eastmoneyParameters.fees = eastmoneyBasicFeeSchedule();
    StrategySimulator eastmoneySimulator(eastmoneyParameters);
    stock.price = 10.0;
    stock.previousClose = 10.0;
    stock.time = "2026-08-08 10:00";
    auto eastmoneyBuy = eastmoneySimulator.processSignal(
        momentum, signal(stock, SignalType::Buy, 10000), stock, nullptr);
    assert(eastmoneyBuy.executed && eastmoneyBuy.trade);
    assert(eastmoneyBuy.trade->fees > 26.0 && eastmoneyBuy.trade->fees < 26.1);

    // 同一研究组合可按证券代码应用不同佣金模板。
    SimulationParameters symbolFeeParameters;
    symbolFeeParameters.initialCashPerSymbol = 200000.0;
    symbolFeeParameters.fees = eastmoneyBasicFeeSchedule();
    SimulationFeeSchedule lowFee = eastmoneyBasicFeeSchedule();
    lowFee.id = "custom_low";
    lowFee.commissionRate = 0.0001;
    lowFee.minimumCommission = 1.0;
    symbolFeeParameters.feesBySymbol["sz000001"] = lowFee;
    StrategySimulator symbolFeeSimulator(symbolFeeParameters);
    Quote secondStock = stock;
    secondStock.symbol = "sz000001";
    auto defaultFeeBuy = symbolFeeSimulator.processSignal(
        momentum, signal(stock, SignalType::Buy, 10000), stock, nullptr);
    auto lowFeeBuy = symbolFeeSimulator.processSignal(
        momentum, signal(secondStock, SignalType::Buy, 10000), secondStock, nullptr);
    assert(defaultFeeBuy.trade && lowFeeBuy.trade);
    assert(defaultFeeBuy.trade->fees > 26.0 && defaultFeeBuy.trade->fees < 26.1);
    assert(lowFeeBuy.trade->fees > 11.0 && lowFeeBuy.trade->fees < 11.1);

    // 第一天高位卖出后未回补，第二天保留待买回数量、现金和持仓市值，
    // 相反方向信号优先按跨日敞口数量闭合。
    SimulationParameters carryParameters;
    carryParameters.initialCashPerSymbol = 10000.0;
    carryParameters.fees.commissionRate = 0.0;
    carryParameters.fees.minimumCommission = 0.0;
    carryParameters.fees.stockStampDutyRate = 0.0;
    carryParameters.fees.stockTransferFeeRate = 0.0;
    carryParameters.fees.slippageRatePerSide = 0.0;
    StrategySimulator carrySimulator(carryParameters);
    fund.price = 1.200;
    fund.previousClose = 1.180;
    fund.time = "2026-08-07 14:00";
    auto carrySell = carrySimulator.processSignal(
        t0, signal(fund, SignalType::Sell, 1000), fund, &fundSeed);
    assert(carrySell.executed && carrySell.trade && carrySell.trade->quantity == 1000);
    auto firstCarryDay = carrySimulator.snapshot(t0.id, fund.symbol, fund.price);
    assert(firstCarryDay && firstCarryDay->quantity == 2000);

    fund.price = 1.180;
    fund.previousClose = 1.200;
    fund.time = "2026-08-08 09:35";
    carrySimulator.ensureAccount(t0, fund, &fundSeed);
    Holding carriedHolding = carrySimulator.holdingContext(t0.id, fund.symbol);
    assert(carriedHolding.quantity == 2000);
    assert(carriedHolding.todaySellQuantity == 1000);
    assert(carriedHolding.todayT0OpenPrice > 0.0);
    auto carryBuy = carrySimulator.processSignal(
        t0, signal(fund, SignalType::Buy, 600), fund, &fundSeed);
    assert(carryBuy.executed && carryBuy.trade && carryBuy.trade->quantity == 1000);
    auto closedCarry = carrySimulator.snapshot(t0.id, fund.symbol, fund.price);
    assert(closedCarry && closedCarry->quantity == 3000);
    assert(nearlyEqual(closedCarry->totalProfit, 20.0));
    assert(nearlyEqual(closedCarry->t0Profit, 20.0));

    std::cout << "Strategy simulator tests passed\n";
    return 0;
}
