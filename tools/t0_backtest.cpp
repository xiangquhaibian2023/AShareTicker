#include "quote_provider.h"
#include "replay_engine.h"
#include "storage.h"
#include "strategy_backtest.h"
#include "trading_algorithm.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string compactDate(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    return value;
}

std::string isoDate(const std::string& value) {
    std::string compact = compactDate(value);
    if (compact.size() != 8) {
        return value;
    }
    return compact.substr(0, 4) + "-" + compact.substr(4, 2) + "-" + compact.substr(6, 2);
}

// 将算法返回的详细观察说明归并为稳定类别，便于比较不同参数的主要拦截原因。
std::string rejectionCategory(const std::string& message) {
    const std::pair<const char*, const char*> categories[] = {
        {"样本不足", "样本不足"},
        {"成交金额过小", "单轮金额不足"},
        {"前不新开", "开盘观察期"},
        {"午间收盘前后", "午间禁开"},
        {"后停止新开", "超过开仓时限"},
        {"流动性门槛", "流动性不足"},
        {"等待价格偏离均值", "偏离或反转不足"},
        {"中窗斜率仍为", "反转后趋势未减弱"},
        {"过滤逆日线方向", "日线方向过滤"},
        {"上一轮刚完成", "重复开仓冷却"},
        {"未达到完整成本", "成本收益不足"},
        {"涨停", "涨跌停保护"},
        {"跌停", "涨跌停保护"},
        {"持仓状态校验失败", "持仓状态异常"},
    };
    for (const auto& [needle, category] : categories) {
        if (message.find(needle) != std::string::npos) return category;
    }
    return message.empty() ? "无说明" : "其他";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        namespace fs = std::filesystem;
        fs::path root = argc > 1 ? fs::u8path(argv[1]) : fs::current_path();
        ashare::HoldingStore holdingStore(root / "holdings.tsv");
        ashare::SymbolSetStore symbolStore(root / "strategy_symbols.txt");
        std::map<std::string, ashare::Holding> holdings = holdingStore.load();
        std::set<std::string> symbols = symbolStore.load();
        if (symbols.empty()) {
            throw std::runtime_error("strategy_symbols.txt 中没有回测标的");
        }

        ashare::T0StrategyParameters parameters = ashare::defaultT0StrategyParameters();
        bool diagnose = false;
        std::vector<std::string> dates;
        for (int index = 2; index < argc; ++index) {
            std::string argument = argv[index];
            auto parseDoubleOption = [&](const char* prefix, double& target) {
                std::string key = prefix;
                if (argument.rfind(key, 0) != 0) return false;
                target = std::stod(argument.substr(key.size()));
                return true;
            };
            auto parseIntOption = [&](const char* prefix, int& target) {
                std::string key = prefix;
                if (argument.rfind(key, 0) != 0) return false;
                target = std::stoi(argument.substr(key.size()));
                return true;
            };
            int sampleCount = static_cast<int>(parameters.sampleCount);
            int meanLookback = static_cast<int>(parameters.meanReversionLookback);
            int reversalLookback =
                static_cast<int>(parameters.reversalConfirmationLookback);
            int reversalBars = parameters.minimumReversalBarsAfterExtreme;
            int recentSlopeLookback =
                static_cast<int>(parameters.trendRecentSlopeLookback);
            if (argument == "--diagnose") {
                diagnose = true;
            } else if (parseIntOption("--sample=", sampleCount)) {
                parameters.sampleCount = static_cast<size_t>(sampleCount);
            } else if (parseIntOption("--mean-lookback=", meanLookback)) {
                parameters.meanReversionLookback = static_cast<size_t>(meanLookback);
            } else if (parseIntOption("--reversal-lookback=", reversalLookback)) {
                parameters.reversalConfirmationLookback =
                    static_cast<size_t>(reversalLookback);
            } else if (parseIntOption("--reversal-bars=", reversalBars)) {
                parameters.minimumReversalBarsAfterExtreme = reversalBars;
            } else if (parseIntOption("--stock-reversal-ticks=",
                                      parameters.minimumStockReversalTicks)) {
                // 参数已写入。
            } else if (parseIntOption("--fund-reversal-ticks=",
                                      parameters.minimumFundReversalTicks)) {
                // 参数已写入。
            } else if (parseIntOption("--recent-slope-lookback=", recentSlopeLookback)) {
                parameters.trendRecentSlopeLookback =
                    static_cast<size_t>(recentSlopeLookback);
            } else if (parseDoubleOption("--deviation=", parameters.minimumDeviation) ||
                       parseDoubleOption("--deviation-vol=", parameters.deviationVolatilityMultiplier) ||
                       parseDoubleOption("--reversal-recovery=", parameters.minimumReversalRecoveryRate) ||
                       parseDoubleOption("--mean-slope-guard=", parameters.maximumMeanCounterSlopePerMinute) ||
                       parseDoubleOption("--trend=", parameters.minimumTrendThreshold) ||
                       parseDoubleOption("--trend-vol=", parameters.trendVolatilityMultiplier) ||
                       parseDoubleOption("--direction=", parameters.trendDirectionRatio) ||
                       parseDoubleOption("--momentum=", parameters.shortMomentumThreshold) ||
                       parseDoubleOption("--slope=", parameters.minimumRegressionSlopePerMinute) ||
                       parseDoubleOption("--slope-vol=", parameters.regressionSlopeVolatilityMultiplier) ||
                       parseDoubleOption("--trend-fit=", parameters.minimumRegressionFit) ||
                       parseDoubleOption("--recent-slope-ratio=", parameters.minimumRecentToLongSlopeRatio) ||
                       parseDoubleOption("--trend-profit=", parameters.minimumTrendProfitRate) ||
                       parseDoubleOption("--trend-profit-max=", parameters.maximumTrendProfitRate) ||
                       parseDoubleOption("--trend-profit-vol=", parameters.trendProfitVolatilityMultiplier) ||
                       parseDoubleOption("--mean-profit=", parameters.minimumMeanProfitRate) ||
                       parseDoubleOption("--window-min=", parameters.minimumLimitWindowRate) ||
                       parseDoubleOption("--window-max=", parameters.maximumLimitWindowRate) ||
                       parseDoubleOption("--window-vol=", parameters.limitWindowVolatilityMultiplier) ||
                       parseDoubleOption("--limit-buffer=", parameters.priceLimitEmergencyBufferRate) ||
                       parseDoubleOption("--slippage-vol=", parameters.transactionCosts.volatilitySlippageMultiplier) ||
                       parseDoubleOption("--slippage-participation=", parameters.transactionCosts.orderParticipationSlippageMultiplier) ||
                       parseDoubleOption("--slippage-max=", parameters.transactionCosts.maximumSlippageRatePerSide) ||
                       parseDoubleOption("--stop=", parameters.stopLossRate) ||
                       parseDoubleOption("--position=", parameters.roundPositionRate) ||
                       parseDoubleOption("--max-position=", parameters.maximumRoundPositionRate) ||
                       parseDoubleOption("--reentry=", parameters.reentryMoveRate) ||
                       parseDoubleOption("--cost-multiple=", parameters.costSafetyMultiple) ||
                       parseDoubleOption("--min-net=", parameters.minimumNetProfit) ||
                       parseDoubleOption("--min-order=", parameters.minimumOrderValue) ||
                       parseDoubleOption("--daily-trend=", parameters.maximumDailyTrendDeviation) ||
                       parseDoubleOption("--session-guard=", parameters.sessionTrendGuardRate) ||
                       parseIntOption("--rounds=", parameters.maximumRoundsPerDay) ||
                       parseIntOption("--entry-end=", parameters.entryEndMinute) ||
                       parseIntOption("--force-close=", parameters.forceCloseMinute)) {
                // 参数已写入。
            } else if (argument.rfind("--", 0) == 0) {
                throw std::runtime_error("未知参数：" + argument);
            } else {
                dates.push_back(compactDate(argument));
            }
        }
        if (dates.empty()) {
            for (const auto& entry : fs::directory_iterator(root / "replay_cache")) {
                if (entry.is_directory()) {
                    dates.push_back(entry.path().filename().string());
                }
            }
            std::sort(dates.begin(), dates.end());
        }

        ashare::StrategyConfig strategy{
            "builtin_t0_intraday", "T0 日内回转", ashare::StrategyKind::T0Intraday};
        const ashare::SimulationFeeSchedule fees =
            ashare::eastmoneyBasicFeeSchedule();
        // 诊断工具与算法研究页使用同一成本口径，避免信号层按低费率放行、
        // 成交层按真实模板扣费后才发现交易不划算。
        parameters.transactionCosts.commissionRate = fees.commissionRate;
        parameters.transactionCosts.minimumCommission = fees.minimumCommission;
        parameters.transactionCosts.stockStampDutyRate = fees.stockStampDutyRate;
        parameters.transactionCosts.stockTransferFeeRate = fees.stockTransferFeeRate;
        parameters.transactionCosts.slippageRatePerSide = fees.slippageRatePerSide;
        auto algorithm = ashare::createT0TradingAlgorithm(strategy.name, parameters);
        std::map<std::string, ashare::Holding> openingHoldings = holdings;
        for (auto& [symbol, holding] : openingHoldings) {
            (void)symbol;
            ashare::clearIntradayTrades(holding);
        }
        ashare::SimulationParameters simulationParameters;
        simulationParameters.initialCashPerSymbol =
            100000.0 / static_cast<double>(symbols.size());
        simulationParameters.fees = fees;
        ashare::StrategySimulator simulator(simulationParameters);
        double aggregateProfit = 0.0;
        double aggregateFees = 0.0;
        int aggregateTrades = 0;
        int aggregateRounds = 0;
        std::map<std::string, double> profitBySymbol;
        std::map<std::string, double> feesBySymbol;
        std::map<std::string, int> tradesBySymbol;
        std::map<std::string, int> roundsBySymbol;
        int profitableAccounts = 0;
        int completedAccounts = 0;
        std::map<std::string, ashare::Quote> finalQuotes;

        std::cout << std::fixed << std::setprecision(2);
        for (const auto& date : dates) {
            ashare::ReplayEngine replay(root / "replay_cache");
            ashare::QuoteProvider provider;
            replay.begin(isoDate(date));
            int loaded = 0;
            for (const auto& symbol : symbols) {
                auto holding = holdings.find(symbol);
                std::string name = holding == holdings.end() ? symbol : holding->second.symbol;
                try {
                    replay.ensureSymbol(symbol, name, provider);
                    ++loaded;
                } catch (const std::exception& ex) {
                    std::cerr << date << ' ' << symbol << " skipped: " << ex.what() << '\n';
                }
            }
            if (loaded == 0) {
                continue;
            }
            if (diagnose) {
                std::map<std::string, std::map<std::string, int>> rejectionCounts;
                std::map<std::string, int> entrySignals;
                replay.reset();
                while (replay.ready()) {
                    for (const auto& symbol : symbols) {
                        auto quote = replay.quote(symbol);
                        auto holding = openingHoldings.find(symbol);
                        if (!quote || holding == openingHoldings.end()) continue;
                        ashare::TradingSignal signal = algorithm->evaluate(
                            *quote, replay.dailyLines(symbol, 80),
                            replay.minuteLines(symbol), &holding->second);
                        if (signal.type == ashare::SignalType::None) {
                            ++rejectionCounts[symbol][rejectionCategory(signal.message)];
                        } else {
                            ++entrySignals[symbol];
                        }
                    }
                    if (replay.finished()) break;
                    replay.advance();
                }
                std::cout << "DIAG " << date << '\n';
                for (const auto& symbol : symbols) {
                    std::vector<std::pair<std::string, int>> ordered(
                        rejectionCounts[symbol].begin(), rejectionCounts[symbol].end());
                    std::sort(ordered.begin(), ordered.end(),
                              [](const auto& left, const auto& right) {
                                  return left.second > right.second;
                              });
                    std::cout << "  " << symbol << " entry_candidates="
                              << entrySignals[symbol];
                    for (size_t index = 0; index < std::min<size_t>(ordered.size(), 5); ++index) {
                        std::cout << " | " << ordered[index].first << '=' << ordered[index].second;
                    }
                    std::cout << '\n';
                }
            }
            size_t tradeOffset = simulator.trades().size();
            auto result = ashare::runStrategyDayWithSimulator(
                strategy, replay, symbols, openingHoldings, simulator,
                algorithm.get());
            for (const auto& [symbol, quote] : result.latestQuotes) {
                finalQuotes[symbol] = quote;
            }
            double dateProfit = 0.0;
            double dateFees = 0.0;
            int dateTrades = 0;
            int dateRounds = 0;
            std::cout << "DATE " << date << '\n';
            std::cout << std::setprecision(4);
            for (const auto& signal : result.signalTransitions) {
                std::cout << "  SIGNAL " << signal.time << ' ' << signal.symbol << ' '
                          << (signal.type == ashare::SignalType::Buy ? "BUY" : "SELL")
                          << " price=" << signal.currentPrice << " net=" << signal.expectedNetProfit
                          << " message=" << signal.message << '\n';
            }
            const auto& trades = simulator.trades();
            for (size_t tradeIndex = tradeOffset; tradeIndex < trades.size();
                 ++tradeIndex) {
                const auto& trade = trades[tradeIndex];
                std::cout << "  TRADE " << trade.time << ' ' << trade.symbol << ' '
                          << (trade.side == ashare::SignalType::Buy ? "BUY" : "SELL")
                          << " qty=" << trade.quantity << " signal=" << trade.signalPrice
                          << " execution=" << trade.executionPrice
                          << " fees=" << trade.fees << '\n';
            }
            std::cout << std::setprecision(2);
            for (const auto& symbol : symbols) {
                auto quote = result.latestQuotes.find(symbol);
                if (quote == result.latestQuotes.end()) {
                    continue;
                }
                auto snapshot = simulator.snapshot(strategy.id, symbol,
                                                   quote->second.price);
                if (!snapshot) {
                    continue;
                }
                int rounds = snapshot->dailyTradeCount / 2;
                if (rounds > 0) {
                    ++completedAccounts;
                    if (snapshot->t0Profit > 0.0) {
                        ++profitableAccounts;
                    }
                }
                dateProfit += snapshot->t0Profit;
                dateFees += snapshot->dailyFees;
                dateTrades += snapshot->dailyTradeCount;
                dateRounds += rounds;
                profitBySymbol[symbol] += snapshot->t0Profit;
                feesBySymbol[symbol] += snapshot->dailyFees;
                tradesBySymbol[symbol] += snapshot->dailyTradeCount;
                roundsBySymbol[symbol] += rounds;
                std::cout << "  " << symbol << " pnl=" << snapshot->t0Profit
                          << " fees=" << snapshot->dailyFees
                          << " trades=" << snapshot->dailyTradeCount
                          << " rounds=" << rounds << '\n';
            }
            std::cout << "  TOTAL pnl=" << dateProfit << " fees=" << dateFees
                      << " trades=" << dateTrades << " rounds=" << dateRounds
                      << " signals=" << result.signalTransitions.size() << '\n';
            aggregateProfit += dateProfit;
            aggregateFees += dateFees;
            aggregateTrades += dateTrades;
            aggregateRounds += dateRounds;
        }
        double finalTotalProfit = 0.0;
        double finalBuyAndHoldProfit = 0.0;
        for (const auto& symbol : symbols) {
            auto opening = openingHoldings.find(symbol);
            auto quote = finalQuotes.find(symbol);
            if (opening == openingHoldings.end() || quote == finalQuotes.end()) continue;
            auto snapshot = simulator.snapshot(strategy.id, symbol,
                                               quote->second.price);
            if (!snapshot) continue;
            finalTotalProfit += snapshot->totalProfit;
            finalBuyAndHoldProfit +=
                static_cast<double>(opening->second.quantity) *
                (quote->second.price - opening->second.cost);
        }
        double winRate = completedAccounts > 0
                             ? 100.0 * static_cast<double>(profitableAccounts) /
                                   static_cast<double>(completedAccounts)
                             : 0.0;
        std::cout << "SYMBOL_TOTALS\n";
        for (const auto& symbol : symbols) {
            std::cout << "  " << symbol << " pnl=" << profitBySymbol[symbol]
                      << " fees=" << feesBySymbol[symbol]
                      << " trades=" << tradesBySymbol[symbol]
                      << " rounds=" << roundsBySymbol[symbol] << '\n';
        }
        std::cout << "ALL pnl=" << aggregateProfit << " fees=" << aggregateFees
                  << " trades=" << aggregateTrades << " rounds=" << aggregateRounds
                  << " profitable_accounts=" << profitableAccounts << '/' << completedAccounts
                  << " win_rate=" << winRate
                  << "% total_profit=" << finalTotalProfit
                  << " buy_hold=" << finalBuyAndHoldProfit
                  << " strategy_incremental="
                  << finalTotalProfit - finalBuyAndHoldProfit << '\n';
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "backtest failed: " << ex.what() << '\n';
        return 1;
    }
}
