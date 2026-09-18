#include "research_analysis.h"

#include "quote_provider.h"
#include "replay_engine.h"
#include "strategy_backtest.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace ashare {
namespace {

std::string displayDateFromToken(const std::string& token) {
    if (token.size() != 8) {
        return token;
    }
    return token.substr(0, 4) + "-" + token.substr(4, 2) + "-" + token.substr(6, 2);
}

bool validDisplayDate(const std::string& date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (size_t index = 0; index < date.size(); ++index) {
        if (index == 4 || index == 7) {
            continue;
        }
        if (date[index] < '0' || date[index] > '9') {
            return false;
        }
    }
    return true;
}

std::optional<size_t> dailyIndex(const std::vector<KLine>& lines,
                                 const std::string& date) {
    for (size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].date.substr(0, 10) == date) {
            return index;
        }
    }
    return std::nullopt;
}

double percentChange(double from, double to) {
    return from > 0.0 ? (to / from - 1.0) * 100.0 : 0.0;
}

double average(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

const TradingSignal* firstSignalForSymbol(
    const std::vector<TradingSignal>& signals, const std::string& symbol) {
    auto found = std::find_if(signals.begin(), signals.end(), [&](const TradingSignal& signal) {
        return signal.symbol == symbol && signal.type != SignalType::None;
    });
    return found == signals.end() ? nullptr : &*found;
}

int signalCountForSymbol(const std::vector<TradingSignal>& signals,
                         const std::string& symbol) {
    return static_cast<int>(std::count_if(signals.begin(), signals.end(),
                                          [&](const TradingSignal& signal) {
                                              return signal.symbol == symbol &&
                                                     signal.type != SignalType::None;
                                          }));
}

void checkCancelled(const ResearchCancelCallback& cancelled) {
    if (cancelled && cancelled()) {
        throw std::runtime_error("算法研究已取消。");
    }
}

const ResearchAccountDayResult* accountResultForSymbol(
    const ResearchDayResult& day, const std::string& symbol) {
    auto found = std::find_if(day.accounts.begin(), day.accounts.end(),
                              [&](const ResearchAccountDayResult& item) {
                                  return item.symbol == symbol;
                              });
    return found == day.accounts.end() ? nullptr : &*found;
}

const ResearchSecurityResult* securityResultForSymbol(
    const ResearchDayResult& day, const std::string& symbol) {
    auto found = std::find_if(day.securities.begin(), day.securities.end(),
                              [&](const ResearchSecurityResult& item) {
                                  return item.symbol == symbol;
                              });
    return found == day.securities.end() ? nullptr : &*found;
}

void buildResearchSymbolSummaries(
    AlgorithmResearchReport& report,
    const AlgorithmResearchRequest& request,
    const std::set<std::string>& simulationSymbols,
    double cashPerSymbol) {
    for (const auto& symbol : simulationSymbols) {
        ResearchSymbolSummary summary;
        summary.symbol = symbol;
        auto name = request.symbols.find(symbol);
        summary.name = name == request.symbols.end() ? symbol : name->second;
        auto opening = request.openingHoldings.find(symbol);
        if (opening != request.openingHoldings.end()) {
            summary.openingQuantity = opening->second.quantity;
            summary.openingCost = opening->second.cost;
        }
        summary.initialCapital = cashPerSymbol +
                                 static_cast<double>(summary.openingQuantity) *
                                     summary.openingCost;

        double signalCapital = 1.0;
        double currentSimulationReturn = 0.0;
        double endingPrice = 0.0;
        for (size_t index = 0; index < report.days.size(); ++index) {
            const auto& day = report.days[index];
            if (const auto* security = securityResultForSymbol(day, symbol)) {
                signalCapital *= 1.0 + security->followupReturnPercent / 100.0;
            }
            if (const auto* account = accountResultForSymbol(day, symbol)) {
                ++summary.activeDays;
                summary.initialCapital = account->initialEquity;
                summary.endingQuantity = account->quantity;
                summary.endingCost = account->cost;
                summary.endingCash = account->cash;
                summary.endingEquity = account->equity;
                endingPrice = account->currentPrice;
                summary.totalProfit = account->totalProfit;
                summary.totalReturnPercent = account->totalReturnPercent;
                summary.t0Profit += account->t0Profit;
                summary.totalFees += account->dailyFees;
                summary.tradeCount += account->dailyTradeCount;
                currentSimulationReturn = account->totalReturnPercent;
            }
            ResearchCurvePoint point;
            point.date = day.signalDate;
            point.signalCumulativePercent = (signalCapital - 1.0) * 100.0;
            point.simulationCumulativePercent = currentSimulationReturn;
            if (index < report.curve.size()) {
                point.benchmarkCumulativePercent =
                    report.curve[index].benchmarkCumulativePercent;
            }
            summary.curve.push_back(point);
        }
        summary.signalCumulativePercent = (signalCapital - 1.0) * 100.0;
        summary.buyAndHoldProfit = summary.openingQuantity > 0 &&
                                           summary.openingCost > 0.0 && endingPrice > 0.0
                                       ? static_cast<double>(summary.openingQuantity) *
                                             (endingPrice - summary.openingCost)
                                       : 0.0;
        summary.strategyIncrementalProfit = summary.totalProfit -
                                            summary.buyAndHoldProfit;
        summary.benchmarkCumulativePercent = summary.curve.empty()
                                                 ? 0.0
                                                 : summary.curve.back()
                                                       .benchmarkCumulativePercent;
        report.buyAndHoldProfit += summary.buyAndHoldProfit;
        report.strategyIncrementalProfit += summary.strategyIncrementalProfit;
        report.symbolSummaries.emplace(symbol, std::move(summary));
    }
}

}  // namespace

AlgorithmResearchReport buildAlgorithmResearchReport(
    const StrategyConfig& strategy, const std::string& startDate,
    const std::string& endDate, const std::string& benchmarkName,
    std::vector<ResearchDayResult> days,
    const std::vector<KLine>& benchmarkDailyLines) {
    AlgorithmResearchReport report;
    report.strategy = strategy;
    report.startDate = startDate;
    report.endDate = endDate;
    report.benchmarkName = benchmarkName;
    std::sort(days.begin(), days.end(), [](const ResearchDayResult& left,
                                           const ResearchDayResult& right) {
        return left.signalDate < right.signalDate;
    });
    report.days = std::move(days);
    report.coveredTradingDays = static_cast<int>(report.days.size());

    double signalCapital = 1.0;
    double simulationCapital = 1.0;
    double benchmarkBase = 0.0;
    for (const auto& line : benchmarkDailyLines) {
        std::string date = line.date.substr(0, 10);
        if (date >= startDate && date <= endDate && line.close > 0.0) {
            benchmarkBase = line.close;
            break;
        }
    }

    for (const auto& day : report.days) {
        if (!day.securities.empty()) {
            ++report.signalDays;
            report.securityResultCount += static_cast<int>(day.securities.size());
            signalCapital *= 1.0 + day.averageFollowupReturnPercent / 100.0;
            simulationCapital *= 1.0 + day.averageSimulationReturnPercent / 100.0;
        }
        if (day.hasCumulativeSimulationReturn) {
            simulationCapital = 1.0 + day.cumulativeSimulationReturnPercent / 100.0;
        }
        double benchmarkClose = benchmarkBase;
        if (auto index = dailyIndex(benchmarkDailyLines, day.signalDate)) {
            benchmarkClose = benchmarkDailyLines[*index].close;
        }
        ResearchCurvePoint point;
        point.date = day.signalDate;
        point.signalCumulativePercent = (signalCapital - 1.0) * 100.0;
        point.simulationCumulativePercent = (simulationCapital - 1.0) * 100.0;
        point.benchmarkCumulativePercent = percentChange(benchmarkBase, benchmarkClose);
        report.curve.push_back(point);
    }

    report.signalCumulativePercent = (signalCapital - 1.0) * 100.0;
    report.simulationCumulativePercent = (simulationCapital - 1.0) * 100.0;
    // 每个标的取研究区间内最后一个有效账户快照，避免停牌或数据缺口日
    // 只汇总当天有行情的标的而漏掉组合中的既有持仓。
    std::map<std::string, ResearchAccountDayResult> latestAccounts;
    for (const auto& day : report.days) {
        for (const auto& account : day.accounts) {
            latestAccounts[account.symbol] = account;
        }
    }
    for (const auto& [symbol, account] : latestAccounts) {
        (void)symbol;
        report.simulationTotalProfit += account.totalProfit;
        report.simulationInitialCapital += account.initialEquity;
    }
    report.benchmarkCumulativePercent = report.curve.empty()
                                             ? 0.0
                                             : report.curve.back().benchmarkCumulativePercent;
    report.signalExcessPercent = report.signalCumulativePercent -
                                 report.benchmarkCumulativePercent;
    report.simulationExcessPercent = report.simulationCumulativePercent -
                                     report.benchmarkCumulativePercent;
    return report;
}

AlgorithmResearchReport runAlgorithmResearch(
    const AlgorithmResearchRequest& request,
    const ResearchProgressCallback& progress,
    const ResearchCancelCallback& cancelled) {
    if (!validDisplayDate(request.startDate) || !validDisplayDate(request.endDate) ||
        request.startDate > request.endDate) {
        throw std::runtime_error("研究日期必须使用 YYYY-MM-DD，且开始日期不能晚于结束日期。");
    }
    const std::string today = displayDateFromToken(currentDateToken());
    if (request.endDate > today) {
        throw std::runtime_error("研究结束日期不能晚于当前日期。");
    }
    if (request.symbols.empty()) {
        throw std::runtime_error("当前研究范围没有可分析的标的。");
    }
    if (request.symbols.size() > 20) {
        throw std::runtime_error("单次算法研究最多支持 20 个标的。");
    }
    if (!std::isfinite(request.initialCash) || request.initialCash < 0.0) {
        throw std::runtime_error("期初账户资金必须是非负数。");
    }
    auto validFees = [](const SimulationFeeSchedule& fees) {
        return std::isfinite(fees.commissionRate) && fees.commissionRate >= 0.0 &&
               std::isfinite(fees.minimumCommission) && fees.minimumCommission >= 0.0 &&
               std::isfinite(fees.stockStampDutyRate) && fees.stockStampDutyRate >= 0.0 &&
               std::isfinite(fees.stockTransferFeeRate) && fees.stockTransferFeeRate >= 0.0 &&
               std::isfinite(fees.slippageRatePerSide) && fees.slippageRatePerSide >= 0.0;
    };
    if (!validFees(request.fees)) {
        throw std::runtime_error("佣金模板包含无效的负数或非数字费率。");
    }
    for (const auto& [symbol, fees] : request.feesBySymbol) {
        if (!validFees(fees)) {
            throw std::runtime_error(symbol + " 的佣金模板包含无效费率。");
        }
    }

    std::set<std::string> simulationSymbols;
    for (const auto& [symbol, name] : request.symbols) {
        (void)name;
        if (request.strategy.kind == StrategyKind::T0Intraday) {
            auto holding = request.openingHoldings.find(symbol);
            if (holding == request.openingHoldings.end() ||
                holding->second.quantity < 100 || holding->second.cost <= 0.0) {
                continue;
            }
        }
        simulationSymbols.insert(symbol);
    }
    if (simulationSymbols.empty()) {
        throw std::runtime_error("当前算法没有可回测标的；T0 策略需要至少 100 股有效期初持仓。");
    }

    std::map<std::string, Holding> openingHoldings = request.openingHoldings;
    for (auto& [symbol, holding] : openingHoldings) {
        (void)symbol;
        clearIntradayTrades(holding);
    }
    double cashPerSymbol = request.initialCash /
                           static_cast<double>(simulationSymbols.size());
    SimulationParameters simulationParameters;
    simulationParameters.initialCashPerSymbol = cashPerSymbol;
    simulationParameters.fees = request.fees;
    simulationParameters.feesBySymbol = request.feesBySymbol;
    StrategySimulator simulator(simulationParameters);

    QuoteProvider provider;
    if (progress) {
        progress(0, 1, "正在加载基准交易日历");
    }
    std::vector<KLine> benchmarkLines =
        provider.fetchHistoricalDailyLines(request.benchmarkSymbol, today, 420);
    std::vector<std::string> tradingDates;
    for (const auto& line : benchmarkLines) {
        std::string date = line.date.substr(0, 10);
        if (date >= request.startDate && date <= request.endDate) {
            tradingDates.push_back(date);
        }
    }
    std::sort(tradingDates.begin(), tradingDates.end());
    tradingDates.erase(std::unique(tradingDates.begin(), tradingDates.end()),
                       tradingDates.end());
    if (tradingDates.empty()) {
        throw std::runtime_error("指定区间内没有可用的真实交易日行情。");
    }
    if (tradingDates.size() > 60) {
        throw std::runtime_error("单次算法研究最多支持 60 个交易日，请缩短日期区间。");
    }

    std::map<std::string, std::vector<KLine>> dailyBySymbol;
    std::vector<ResearchIssue> issues;
    int totalWork = static_cast<int>(request.symbols.size() * (tradingDates.size() + 1));
    int completed = 0;
    for (const auto& [symbol, name] : request.symbols) {
        (void)name;
        checkCancelled(cancelled);
        try {
            dailyBySymbol[symbol] = provider.fetchHistoricalDailyLines(symbol, today, 420);
        } catch (const std::exception& error) {
            issues.push_back({request.startDate + " ~ " + request.endDate,
                              symbol, "日线加载", error.what()});
        }
        ++completed;
        if (progress) {
            progress(completed, totalWork, "已加载 " + symbol + " 的日线数据");
        }
    }

    std::vector<ResearchDayResult> days;
    days.reserve(tradingDates.size());
    std::map<std::string, ResearchAccountDayResult> latestAccountSnapshots;
    for (const std::string& date : tradingDates) {
        checkCancelled(cancelled);
        ResearchDayResult day;
        day.signalDate = date;
        if (auto benchmarkIndex = dailyIndex(benchmarkLines, date)) {
            if (*benchmarkIndex + 1 < benchmarkLines.size()) {
                const KLine& current = benchmarkLines[*benchmarkIndex];
                const KLine& next = benchmarkLines[*benchmarkIndex + 1];
                day.followupDate = next.date.substr(0, 10);
                day.benchmarkFollowupReturnPercent =
                    percentChange(current.close, (next.high + next.close) / 2.0);
            }
        }

        ReplayEngine replay(request.replayCacheDirectory);
        replay.begin(date);
        std::set<std::string> loadedSymbols;
        for (const auto& [symbol, name] : request.symbols) {
            checkCancelled(cancelled);
            if (simulationSymbols.find(symbol) == simulationSymbols.end()) {
                ++completed;
                continue;
            }
            auto daily = dailyBySymbol.find(symbol);
            if (daily == dailyBySymbol.end() || daily->second.empty()) {
                ++completed;
                continue;
            }
            // 基准指数交易但标的没有日线，通常是停牌或尚未上市，不属于行情错误。
            if (!dailyIndex(daily->second, date)) {
                ++completed;
                continue;
            }
            try {
                replay.ensureSymbol(symbol, name, provider, daily->second);
                loadedSymbols.insert(symbol);
            } catch (const std::exception& error) {
                issues.push_back({date, symbol, "分钟行情", error.what()});
            }
            ++completed;
            if (progress) {
                progress(completed, totalWork,
                         "正在分析 " + date + "  " + symbol);
            }
        }

        std::vector<double> followupReturns;
        std::vector<double> simulationReturns;
        if (!loadedSymbols.empty()) {
            StrategyDayReplayResult backtest = runStrategyDayWithSimulator(
                request.strategy, replay, loadedSymbols, openingHoldings, simulator,
                nullptr, &request.feesBySymbol);
            day.technicalSwingResults = std::move(backtest.technicalSwingResults);

            // 日级汇总之外保留全部买卖信号，具体日期筛选时可直接展示
            // 信号价、有效数量以及对应的模拟成交结果。
            for (const auto& execution : backtest.signalExecutions) {
                ResearchTradeSignalDetail detail;
                detail.date = date;
                detail.time = execution.signal.time;
                detail.symbol = execution.signal.symbol;
                auto symbolName = request.symbols.find(detail.symbol);
                detail.name = symbolName == request.symbols.end()
                                  ? execution.signal.name
                                  : symbolName->second;
                detail.algorithm = execution.signal.algorithm.empty()
                                       ? request.strategy.name
                                       : execution.signal.algorithm;
                detail.direction = execution.signal.type;
                detail.signalPrice = execution.signal.currentPrice;
                detail.signalQuantity = execution.signal.suggestedQuantity;
                detail.executed = execution.executed;
                detail.description = execution.signal.message;
                if (!execution.executionMessage.empty()) {
                    if (!detail.description.empty()) detail.description += "；";
                    detail.description += execution.executionMessage;
                }
                if (execution.trade) {
                    detail.executionPrice = execution.trade->executionPrice;
                    detail.executionQuantity = execution.trade->quantity;
                    detail.fees = execution.trade->fees;
                    // 非 T0 算法可能由账户资金自动计算数量，此时实际模拟数量
                    // 就是该信号在当前账户参数下的有效建议数量。
                    if (detail.signalQuantity <= 0) {
                        detail.signalQuantity = detail.executionQuantity;
                    }
                }
                day.tradeSignals.push_back(std::move(detail));
            }

            for (const auto& symbol : loadedSymbols) {
                auto quote = backtest.latestQuotes.find(symbol);
                if (quote == backtest.latestQuotes.end()) {
                    continue;
                }
                auto snapshot = simulator.snapshot(request.strategy.id, symbol,
                                                   quote->second.price);
                if (!snapshot) {
                    continue;
                }
                ResearchAccountDayResult account;
                account.date = date;
                account.symbol = symbol;
                auto symbolName = request.symbols.find(symbol);
                account.name = symbolName == request.symbols.end() ? symbol
                                                                   : symbolName->second;
                account.quantity = snapshot->quantity;
                account.cost = snapshot->cost;
                account.cash = snapshot->cash;
                account.currentPrice = quote->second.price;
                account.marketValue = snapshot->marketValue;
                account.equity = snapshot->equity;
                account.initialEquity = snapshot->initialEquity;
                account.dailyProfit = snapshot->dailyProfit;
                account.totalProfit = snapshot->totalProfit;
                account.totalReturnPercent = snapshot->totalReturnPercent;
                account.t0Profit = snapshot->t0Profit;
                account.dailyFees = snapshot->dailyFees;
                account.dailyTradeCount = snapshot->dailyTradeCount;
                simulationReturns.push_back(
                    snapshot->openingEquity > 0.0
                        ? snapshot->dailyProfit / snapshot->openingEquity * 100.0
                        : 0.0);
                day.accounts.push_back(std::move(account));
            }

            for (const auto& symbol : loadedSymbols) {
                const TradingSignal* signal = firstSignalForSymbol(
                    backtest.signalTransitions, symbol);
                if (!signal) {
                    continue;
                }
                auto daily = dailyBySymbol.find(symbol);
                auto index = daily == dailyBySymbol.end()
                                 ? std::optional<size_t>{}
                                 : dailyIndex(daily->second, date);
                if (!index || *index + 1 >= daily->second.size()) {
                    continue;
                }
                const KLine& signalDay = daily->second[*index];
                const KLine& followupDay = daily->second[*index + 1];
                if (signalDay.close <= 0.0 || followupDay.high <= 0.0 ||
                    followupDay.close <= 0.0) {
                    continue;
                }
                double previousClose = *index > 0 ? daily->second[*index - 1].close
                                                  : signalDay.open;
                ResearchSecurityResult item;
                item.signalDate = date;
                item.followupDate = followupDay.date.substr(0, 10);
                item.symbol = symbol;
                auto symbolName = request.symbols.find(symbol);
                item.name = symbolName == request.symbols.end() ? symbol
                                                                : symbolName->second;
                item.algorithm = request.strategy.name;
                item.direction = signal->type;
                item.signalDayChangePercent = percentChange(previousClose, signalDay.close);
                item.basePrice = signalDay.close;
                item.followupHigh = followupDay.high;
                item.followupClose = followupDay.close;
                item.comparisonPrice = (followupDay.high + followupDay.close) / 2.0;
                double rawReturn = percentChange(item.basePrice, item.comparisonPrice);
                item.followupReturnPercent = signal->type == SignalType::Sell
                                                 ? -rawReturn : rawReturn;
                item.benchmarkReturnPercent = day.benchmarkFollowupReturnPercent;
                item.excessReturnPercent = item.followupReturnPercent -
                                           item.benchmarkReturnPercent;
                item.signalCount = signalCountForSymbol(backtest.signalTransitions, symbol);
                auto snapshotQuote = backtest.latestQuotes.find(symbol);
                auto snapshot = snapshotQuote == backtest.latestQuotes.end()
                                    ? std::optional<SimulationSnapshot>{}
                                    : simulator.snapshot(request.strategy.id, symbol,
                                                         snapshotQuote->second.price);
                item.tradeCount = snapshot ? snapshot->dailyTradeCount : 0;
                item.fees = snapshot ? snapshot->dailyFees : 0.0;
                auto quote = backtest.latestQuotes.find(symbol);
                if (quote != backtest.latestQuotes.end()) {
                    snapshot = simulator.snapshot(
                        request.strategy.id, symbol, quote->second.price);
                    if (snapshot) {
                        double openingEquity = snapshot->openingEquity;
                        item.simulationReturnPercent = openingEquity > 0.0
                                                           ? snapshot->dailyProfit /
                                                                 openingEquity * 100.0
                                                           : 0.0;
                        item.simulationProfit = snapshot->dailyProfit;
                        item.t0Profit = snapshot->t0Profit;
                    }
                }
                followupReturns.push_back(item.followupReturnPercent);
                day.securities.push_back(std::move(item));
            }
        }
        day.averageFollowupReturnPercent = average(followupReturns);
        day.averageSimulationReturnPercent = average(simulationReturns);
        for (const auto& account : day.accounts) {
            latestAccountSnapshots[account.symbol] = account;
        }
        double portfolioInitialEquity = 0.0;
        double portfolioTotalProfit = 0.0;
        for (const auto& [symbol, account] : latestAccountSnapshots) {
            (void)symbol;
            portfolioInitialEquity += account.initialEquity;
            portfolioTotalProfit += account.totalProfit;
        }
        if (portfolioInitialEquity > 0.0) {
            day.cumulativeSimulationReturnPercent =
                portfolioTotalProfit / portfolioInitialEquity * 100.0;
            day.hasCumulativeSimulationReturn = true;
        }
        days.push_back(std::move(day));
    }

    AlgorithmResearchReport report = buildAlgorithmResearchReport(
        request.strategy, request.startDate, request.endDate,
        request.benchmarkName, std::move(days), benchmarkLines);
    buildResearchSymbolSummaries(report, request, simulationSymbols, cashPerSymbol);
    report.issues = std::move(issues);
    return report;
}

}  // namespace ashare
