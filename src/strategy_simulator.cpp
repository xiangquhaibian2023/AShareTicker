#include "strategy_simulator.h"

#include "utils.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ashare {
namespace {

constexpr double defaultOrderBudgetRatio = 0.20; // 无建议数量时使用 20% 期初模拟现金。

struct TradeCost {
    double executionPrice = 0.0;
    double fees = 0.0;
    double cashChange = 0.0;
};

TradeCost estimateTrade(double signalPrice, long long quantity, bool buy, bool fundLike,
                        const SimulationFeeSchedule& fees) {
    TradeCost result;
    if (signalPrice <= 0.0 || quantity <= 0) {
        return result;
    }
    result.executionPrice = signalPrice *
                            (buy ? 1.0 + fees.slippageRatePerSide
                                 : 1.0 - fees.slippageRatePerSide);
    double notional = result.executionPrice * static_cast<double>(quantity);
    double commission = std::max(fees.minimumCommission,
                                 notional * fees.commissionRate);
    double transferFee = fundLike ? 0.0 : notional * fees.stockTransferFeeRate;
    double stampDuty = !buy && !fundLike ? notional * fees.stockStampDutyRate : 0.0;
    result.fees = commission + transferFee + stampDuty;
    result.cashChange = buy ? -(notional + result.fees) : notional - result.fees;
    return result;
}

}  // namespace

SimulationFeeSchedule eastmoneyBasicFeeSchedule() {
    SimulationFeeSchedule result;
    result.id = "eastmoney_basic";
    result.name = "东方财富基础账户（万 2.5）";
    result.commissionRate = 0.00025;
    result.minimumCommission = 5.0;
    result.stockStampDutyRate = 0.0005;
    result.stockTransferFeeRate = 0.00001;
    result.slippageRatePerSide = 0.0002;
    return result;
}

StrategySimulator::StrategySimulator(SimulationParameters parameters)
    : parameters_(std::move(parameters)) {
    if (!std::isfinite(parameters_.initialCashPerSymbol) ||
        parameters_.initialCashPerSymbol < 0.0) {
        parameters_.initialCashPerSymbol = initialCashPerSymbol;
    }
}

void StrategySimulator::reset() {
    accounts_.clear();
    trades_.clear();
}

std::string StrategySimulator::accountKey(const std::string& strategyId,
                                          const std::string& symbol) {
    return strategyId + "|" + symbol;
}

long long StrategySimulator::roundToLot(long long quantity) {
    return quantity > 0 ? quantity / 100 * 100 : 0;
}

StrategySimulator::Account& StrategySimulator::account(const StrategyConfig& strategy,
                                                       const Quote& quote,
                                                       const Holding* initialHolding) {
    const std::string key = accountKey(strategy.id, quote.symbol);
    auto existing = accounts_.find(key);
    if (existing != accounts_.end()) {
        beginTradingDay(existing->second, quote);
        return existing->second;
    }

    Account created;
    created.strategyId = strategy.id;
    created.algorithm = strategy.name;
    created.symbol = quote.symbol;
    created.kind = strategy.kind;
    created.tradingDate = quote.time.size() >= 10 ? quote.time.substr(0, 10) : std::string{};
    created.cash = parameters_.initialCashPerSymbol;
    created.initialEquity = parameters_.initialCashPerSymbol;
    created.openingEquity = parameters_.initialCashPerSymbol;
    if (initialHolding && initialHolding->quantity > 0 && initialHolding->cost > 0.0) {
        created.openingQuantity = initialHolding->quantity;
        created.quantity = initialHolding->quantity;
        created.cost = initialHolding->cost;
        created.t0CompletedRounds = initialHolding->todayT0CompletedRounds;
        created.t0OpenPrice = initialHolding->todayT0OpenPrice;
        created.t0LastClosePrice = initialHolding->todayT0LastClosePrice;
        created.initialEquity += static_cast<double>(created.quantity) * created.cost;
        created.openingEquity += static_cast<double>(created.quantity) * quote.previousClose;
    }
    return accounts_.emplace(key, std::move(created)).first->second;
}

void StrategySimulator::beginTradingDay(Account& target, const Quote& quote) {
    std::string date = quote.time.size() >= 10 ? quote.time.substr(0, 10) : std::string{};
    if (date.empty() || date == target.tradingDate) {
        return;
    }
    double referencePrice = quote.previousClose > 0.0 ? quote.previousClose : quote.price;
    target.tradingDate = date;
    target.openingEquity = target.cash +
                           static_cast<double>(target.quantity) * referencePrice;
    target.openingQuantity = target.quantity;
    // T0 未平腿不能在换日时丢失。净买入表示次日仍需卖出，净卖出表示
    // 次日仍需买回；实际持仓和现金已经包含上一日成交，因此只需保留
    // 待平数量、开仓价及净现金流，后续相反信号会优先闭合该敞口。
    long long carriedOutstanding = target.kind == StrategyKind::T0Intraday
                                       ? target.todayBuyQuantity - target.todaySellQuantity
                                       : 0;
    target.todayBuyQuantity = std::max(0LL, carriedOutstanding);
    target.todaySellQuantity = std::max(0LL, -carriedOutstanding);
    target.todayBuyValue = target.todayBuyQuantity > 0
                               ? target.t0OpenPrice * target.todayBuyQuantity
                               : 0.0;
    target.todaySellValue = target.todaySellQuantity > 0
                                ? target.t0OpenPrice * target.todaySellQuantity
                                : 0.0;
    target.t0CompletedRounds = 0;
    target.t0LastClosePrice = 0.0;
    if (carriedOutstanding == 0) {
        target.t0OpenPrice = 0.0;
        target.t0RoundCashFlow = 0.0;
    }
    target.t0Profit = 0.0;
    target.dailyFees = 0.0;
    target.dailyTradeCount = 0;
    target.lastSignal = SignalType::None;
}

const SimulationFeeSchedule& StrategySimulator::feeSchedule(
    const std::string& symbol) const {
    auto configured = parameters_.feesBySymbol.find(symbol);
    return configured == parameters_.feesBySymbol.end() ? parameters_.fees
                                                         : configured->second;
}

void StrategySimulator::ensureAccount(const StrategyConfig& strategy, const Quote& quote,
                                      const Holding* initialHolding) {
    (void)account(strategy, quote, initialHolding);
}

Holding StrategySimulator::holdingContext(const std::string& strategyId,
                                          const std::string& symbol) const {
    Holding holding;
    holding.symbol = symbol;
    auto found = accounts_.find(accountKey(strategyId, symbol));
    if (found == accounts_.end()) {
        return holding;
    }
    const Account& source = found->second;
    holding.quantity = source.quantity;
    holding.cost = source.cost;
    // T0 算法用当前日期识别“今日买入”，这里故意映射为当前运行日以启用可卖数量校验。
    holding.todayTradeDate = currentDateToken();
    holding.todayBuyQuantity = source.todayBuyQuantity;
    holding.todayBuyPrice = source.todayBuyQuantity > 0
                                ? source.todayBuyValue / static_cast<double>(source.todayBuyQuantity)
                                : 0.0;
    holding.todaySellQuantity = source.todaySellQuantity;
    holding.todaySellPrice = source.todaySellQuantity > 0
                                 ? source.todaySellValue / static_cast<double>(source.todaySellQuantity)
                                 : 0.0;
    holding.todayFees = source.dailyFees;
    holding.todayT0CompletedRounds = source.t0CompletedRounds;
    holding.todayT0OpenPrice = source.t0OpenPrice;
    holding.todayT0LastClosePrice = source.t0LastClosePrice;
    return holding;
}

SimulationExecution StrategySimulator::processSignal(const StrategyConfig& strategy,
                                                     const TradingSignal& signal,
                                                     const Quote& quote,
                                                     const Holding* initialHolding) {
    Account& target = account(strategy, quote, initialHolding);
    SimulationExecution result;
    if (signal.type == SignalType::None) {
        target.lastSignal = SignalType::None;
        return result;
    }
    result.signalTransition = signal.type != target.lastSignal;
    target.lastSignal = signal.type;
    if (!result.signalTransition) {
        return result;
    }
    if (signal.currentPrice <= 0.0 || !std::isfinite(signal.currentPrice)) {
        result.message = "模拟委托价格无效";
        return result;
    }

    const bool fundLike = isFundLikeSymbol(signal.symbol);
    const SimulationFeeSchedule& fees = feeSchedule(signal.symbol);
    const long long oldT0Outstanding = target.todayBuyQuantity - target.todaySellQuantity;
    long long quantity = 0;
    if (signal.type == SignalType::Buy) {
        long long outstandingT0Sell = std::max(0LL, target.todaySellQuantity - target.todayBuyQuantity);
        if (strategy.kind == StrategyKind::T0Intraday && outstandingT0Sell > 0) {
            quantity = outstandingT0Sell;
        } else if (signal.suggestedQuantity > 0) {
            quantity = signal.suggestedQuantity;
        } else {
            double orderBudget = parameters_.initialCashPerSymbol *
                                 defaultOrderBudgetRatio;
            quantity = static_cast<long long>(std::min(orderBudget, target.cash) /
                                              (signal.currentPrice *
                                               (1.0 + fees.slippageRatePerSide)));
        }
        quantity = roundToLot(quantity);
        while (quantity >= 100) {
            TradeCost candidate = estimateTrade(signal.currentPrice, quantity, true, fundLike,
                                                fees);
            if (-candidate.cashChange <= target.cash) {
                break;
            }
            quantity -= 100;
        }
        if (quantity < 100) {
            result.message = "模拟资金不足，买入委托未成交";
            return result;
        }
    } else {
        // 普通股票遵守 T+1：仅期初持仓可卖；基金类标的按日内回转模型使用当前持仓。
        long long sellable = fundLike
                                 ? target.quantity
                                 : std::max(0LL, target.openingQuantity - target.todaySellQuantity);
        long long outstandingT0Buy = std::max(0LL, target.todayBuyQuantity - target.todaySellQuantity);
        if (strategy.kind == StrategyKind::T0Intraday && outstandingT0Buy > 0) {
            quantity = outstandingT0Buy;
        } else if (signal.suggestedQuantity > 0) {
            quantity = signal.suggestedQuantity;
        } else {
            quantity = sellable;
        }
        quantity = roundToLot(std::min(quantity, sellable));
        if (quantity < 100) {
            result.message = "无可卖持仓，卖出委托未成交（普通股票执行 T+1）";
            return result;
        }
    }

    const bool buy = signal.type == SignalType::Buy;
    TradeCost costs = estimateTrade(signal.currentPrice, quantity, buy, fundLike,
                                    fees);
    double notional = costs.executionPrice * static_cast<double>(quantity);
    if (buy) {
        double existingCostValue = target.cost * static_cast<double>(target.quantity);
        target.cash += costs.cashChange;
        target.quantity += quantity;
        target.todayBuyQuantity += quantity;
        target.todayBuyValue += notional;
        target.cost = (existingCostValue + notional + costs.fees) /
                      static_cast<double>(target.quantity);
    } else {
        target.cash += costs.cashChange;
        target.quantity -= quantity;
        target.todaySellQuantity += quantity;
        target.todaySellValue += notional;
        target.realizedProfit += (costs.executionPrice - target.cost) *
                                     static_cast<double>(quantity) - costs.fees;
        if (target.quantity == 0) {
            target.cost = 0.0;
        }
    }
    if (strategy.kind == StrategyKind::T0Intraday) {
        // 一轮 T0 的净收益等于两侧成交现金流之和，天然包含所有交易费用和滑点。
        if (oldT0Outstanding == 0) {
            target.t0RoundCashFlow = 0.0;
        }
        target.t0RoundCashFlow += costs.cashChange;
    }
    target.dailyFees += costs.fees;
    target.totalFees += costs.fees;
    if (strategy.kind == StrategyKind::T0Intraday) {
        long long newT0Outstanding = target.todayBuyQuantity - target.todaySellQuantity;
        if (oldT0Outstanding == 0 && newT0Outstanding != 0) {
            target.t0OpenPrice = costs.executionPrice;
        } else if (oldT0Outstanding != 0 && newT0Outstanding == 0) {
            ++target.t0CompletedRounds;
            target.t0Profit += target.t0RoundCashFlow;
            target.t0RoundCashFlow = 0.0;
            target.t0OpenPrice = 0.0;
            target.t0LastClosePrice = costs.executionPrice;
        }
    }
    ++target.dailyTradeCount;
    ++target.tradeCount;

    SimulatedTrade trade;
    trade.strategyId = strategy.id;
    trade.algorithm = strategy.name;
    trade.symbol = signal.symbol;
    trade.side = signal.type;
    trade.quantity = quantity;
    trade.signalPrice = signal.currentPrice;
    trade.executionPrice = costs.executionPrice;
    trade.fees = costs.fees;
    trade.time = signal.time;
    trades_.push_back(trade);
    result.executed = true;
    result.trade = trade;

    std::ostringstream message;
    message.setf(std::ios::fixed);
    message.precision(isFundLikeSymbol(signal.symbol) ? 3 : 2);
    message << "模拟" << (buy ? "买入" : "卖出") << ' ' << quantity
            << " 股，成交价 " << costs.executionPrice;
    message.precision(2);
    message << "，费用 " << costs.fees << " 元";
    result.message = message.str();
    return result;
}

std::optional<SimulationSnapshot> StrategySimulator::snapshot(const std::string& strategyId,
                                                              const std::string& symbol,
                                                              double currentPrice) const {
    auto found = accounts_.find(accountKey(strategyId, symbol));
    if (found == accounts_.end()) {
        return std::nullopt;
    }
    const Account& source = found->second;
    SimulationSnapshot result;
    result.strategyId = strategyId;
    result.symbol = symbol;
    result.quantity = source.quantity;
    result.cost = source.cost;
    result.cash = source.cash;
    result.marketValue = static_cast<double>(source.quantity) * currentPrice;
    result.equity = source.cash + result.marketValue;
    result.initialEquity = source.initialEquity;
    result.openingEquity = source.openingEquity;
    result.dailyProfit = result.equity - source.openingEquity;
    result.totalProfit = result.equity - source.initialEquity;
    result.totalReturnPercent = source.initialEquity > 0.0
                                    ? result.totalProfit / source.initialEquity * 100.0
                                    : 0.0;
    result.realizedProfit = source.realizedProfit;
    result.t0Profit = source.t0Profit;
    result.dailyFees = source.dailyFees;
    result.totalFees = source.totalFees;
    result.dailyTradeCount = source.dailyTradeCount;
    result.tradeCount = source.tradeCount;
    return result;
}

SimulationSummary StrategySimulator::summary(const std::string& strategyId,
                                             const std::map<std::string, Quote>& quotes) const {
    SimulationSummary result;
    double initialEquity = 0.0;
    for (const auto& [key, source] : accounts_) {
        (void)key;
        if (source.strategyId != strategyId) {
            continue;
        }
        auto quote = quotes.find(source.symbol);
        double price = quote == quotes.end() ? 0.0 : quote->second.price;
        auto current = snapshot(strategyId, source.symbol, price);
        if (!current) {
            continue;
        }
        result.equity += current->equity;
        result.dailyProfit += current->dailyProfit;
        result.totalProfit += current->totalProfit;
        result.t0Profit += current->t0Profit;
        result.totalFees += current->totalFees;
        result.quantity += current->quantity;
        result.tradeCount += current->tradeCount;
        initialEquity += source.initialEquity;
    }
    result.totalReturnPercent = initialEquity > 0.0
                                    ? result.totalProfit / initialEquity * 100.0
                                    : 0.0;
    return result;
}

const std::vector<SimulatedTrade>& StrategySimulator::trades() const {
    return trades_;
}

}  // namespace ashare
