#include "strategy_backtest.h"

#include "replay_engine.h"
#include "trading_algorithm.h"
#include "technical_swing_strategy.h"

#include <algorithm>
#include <utility>

namespace ashare {

std::map<std::string, Holding> makeOpeningHoldings(
    const std::map<std::string, Holding>& currentHoldings,
    const std::string& replayDate) {
    std::map<std::string, Holding> result = currentHoldings;
    for (auto& [symbol, holding] : result) {
        (void)symbol;
        if (holding.todayTradeDate == replayDate) {
            holding.quantity = std::max(0LL, holding.quantity - holding.todayBuyQuantity +
                                                 holding.todaySellQuantity);
        }
        holding.todayTradeDate.clear();
        holding.todayBuyQuantity = 0;
        holding.todayBuyPrice = 0.0;
        holding.todaySellQuantity = 0;
        holding.todaySellPrice = 0.0;
        holding.todayFees = 0.0;
        holding.todayT0CompletedRounds = 0;
        holding.todayT0OpenPrice = 0.0;
        holding.todayT0LastClosePrice = 0.0;
    }
    return result;
}

StrategyDayBacktestResult runStrategyDayBacktest(
    const StrategyConfig& strategy, ReplayEngine& replay,
    const std::set<std::string>& symbols,
    const std::map<std::string, Holding>& openingHoldings,
    const TradingAlgorithm* algorithmOverride) {
    StrategyDayBacktestResult result;
    StrategyDayReplayResult replayResult = runStrategyDayWithSimulator(
        strategy, replay, symbols, openingHoldings, result.simulator,
        algorithmOverride);
    result.signalTransitions = std::move(replayResult.signalTransitions);
    result.signalExecutions = std::move(replayResult.signalExecutions);
    result.latestQuotes = std::move(replayResult.latestQuotes);
    result.technicalSwingResults = std::move(replayResult.technicalSwingResults);
    return result;
}

StrategyDayReplayResult runStrategyDayWithSimulator(
    const StrategyConfig& strategy, ReplayEngine& replay,
    const std::set<std::string>& symbols,
    const std::map<std::string, Holding>& openingHoldings,
    StrategySimulator& simulator,
    const TradingAlgorithm* algorithmOverride,
    const std::map<std::string, SimulationFeeSchedule>* feesBySymbol) {
    StrategyDayReplayResult result;
    auto ownedAlgorithm = algorithmOverride ? nullptr : createTradingAlgorithm(strategy);
    std::map<std::string, std::unique_ptr<TradingAlgorithm>> symbolAlgorithms;
    if (!algorithmOverride && strategy.kind == StrategyKind::T0Intraday && feesBySymbol) {
        for (const auto& symbol : symbols) {
            auto configured = feesBySymbol->find(symbol);
            if (configured == feesBySymbol->end()) continue;
            T0StrategyParameters parameters = defaultT0StrategyParameters();
            parameters.transactionCosts.commissionRate = configured->second.commissionRate;
            parameters.transactionCosts.minimumCommission = configured->second.minimumCommission;
            parameters.transactionCosts.stockStampDutyRate =
                configured->second.stockStampDutyRate;
            parameters.transactionCosts.stockTransferFeeRate =
                configured->second.stockTransferFeeRate;
            parameters.transactionCosts.slippageRatePerSide =
                configured->second.slippageRatePerSide;
            symbolAlgorithms.emplace(
                symbol, withTechnicalSwing(createT0TradingAlgorithm(strategy.name, parameters),
                                           strategy.technicalSwing));
        }
    }
    replay.reset();

    while (replay.ready()) {
        for (const auto& symbol : symbols) {
            auto quote = replay.quote(symbol);
            if (!quote) {
                continue;
            }
            result.latestQuotes[symbol] = *quote;

            auto initial = openingHoldings.find(symbol);
            const Holding* initialHolding = initial == openingHoldings.end()
                                                ? nullptr
                                                : &initial->second;
            simulator.ensureAccount(strategy, *quote, initialHolding);
            Holding simulatedHolding = simulator.holdingContext(strategy.id, symbol);
            const Holding* holdingContext = initialHolding ? &simulatedHolding : nullptr;
            const TradingAlgorithm* activeAlgorithm = algorithmOverride
                                                          ? algorithmOverride
                                                          : ownedAlgorithm.get();
            auto symbolAlgorithm = symbolAlgorithms.find(symbol);
            if (symbolAlgorithm != symbolAlgorithms.end()) {
                activeAlgorithm = symbolAlgorithm->second.get();
            }
            TradingSignal signal = activeAlgorithm->evaluate(
                *quote, replay.dailyLines(symbol,
                    strategy.technicalSwing.enableTechnicalSwingStrategy ? 250 : 80), replay.minuteLines(symbol),
                holdingContext);
            if (signal.technicalSwingResult) result.technicalSwingResults[symbol] = *signal.technicalSwingResult;
            SimulationExecution execution = simulator.processSignal(
                strategy, signal, *quote, initialHolding);
            if (execution.signalTransition && signal.type != SignalType::None) {
                StrategySignalExecution detail;
                detail.signal = signal;
                detail.executed = execution.executed;
                detail.trade = execution.trade;
                detail.executionMessage = execution.message;
                result.signalExecutions.push_back(std::move(detail));
                if (!execution.message.empty()) {
                    if (!signal.message.empty()) {
                        signal.message += "；";
                    }
                    signal.message += execution.message;
                }
                result.signalTransitions.push_back(std::move(signal));
            }
        }
        if (replay.finished()) {
            break;
        }
        replay.advance();
    }
    return result;
}

}  // namespace ashare
