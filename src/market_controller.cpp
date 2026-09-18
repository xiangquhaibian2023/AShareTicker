#include "market_controller.h"

#include "dashboard_view.h"
#include "main_window.h"
#include "resource_ids.h"
#include "strategy_backtest.h"
#include "trade_ledger.h"
#include "trading_algorithm.h"
#include "technical_swing_strategy.h"
#include "utils.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ashare {

namespace {

constexpr auto STRATEGY_DAILY_CACHE_LIFETIME = std::chrono::minutes(10);
constexpr auto STRATEGY_MINUTE_CACHE_LIFETIME = std::chrono::seconds(30);
constexpr int FULL_TRADING_MINUTE_COUNT = 242;
constexpr int INCREMENTAL_MINUTE_TAIL_COUNT = 8;

std::string isoDateToken(const std::string& compact) {
    if (compact.size() != 8) {
        return compact;
    }
    return compact.substr(0, 4) + "-" + compact.substr(4, 2) + "-" + compact.substr(6, 2);
}

std::set<std::string> requestedSymbolsForView(const AppState& state) {
    std::set<std::string> requested;
    if (state.detailView == DetailView::Favorites) {
        requested.insert(majorIndexSymbols().begin(), majorIndexSymbols().end());
        requested.insert(state.favorites.begin(), state.favorites.end());
    } else if (state.detailView == DetailView::Holdings) {
        for (const auto& [symbol, holding] : state.holdings) {
            (void)holding;
            requested.insert(symbol);
        }
    } else {
        requested.insert(state.strategySymbols.begin(), state.strategySymbols.end());
    }
    return requested;
}

std::string symbolName(const AppState& state, const std::string& symbol) {
    auto found = state.symbolNames.find(symbol);
    return found == state.symbolNames.end() ? symbol : found->second;
}

void setReplayPlaying(AppState& state, bool playing) {
    state.replayPlaying = playing;
    SetWindowTextW(GetDlgItem(state.window, IDC_REPLAY_PLAY), playing ? L"暂停" : L"播放");
    if (playing) {
        SetTimer(state.window, REPLAY_TIMER, 700, nullptr);
    } else {
        KillTimer(state.window, REPLAY_TIMER);
    }
    InvalidateRect(GetDlgItem(state.window, IDC_REPLAY_PLAY), nullptr, TRUE);
}

void ensureReplaySymbol(AppState& state, const std::string& symbol) {
    bool technicalEnabled = std::any_of(state.strategies.begin(), state.strategies.end(),
        [](const StrategyConfig& c) { return c.technicalSwing.enableTechnicalSwingStrategy; });
    state.replay.ensureSymbol(symbol, symbolName(state, symbol), state.provider, technicalEnabled ? 250 : 81);
}

std::wstring replayProgressText(const AppState& state) {
    return L"回放 " + utf8ToWide(state.replay.date()) + L" " +
           utf8ToWide(state.replay.currentTime()) + L"  " +
           std::to_wstring(state.replay.cursor() + 1) + L"/" +
           std::to_wstring(state.replay.totalMinutes());
}

void captureStrategySignals(AppState& state) {
    const std::string date = currentDateToken();
    if (state.dataMode == MarketDataMode::Live && state.strategySignalDate != date) {
        state.strategySignalHistory.reset();
        state.shownAlertKeys.clear();
        state.strategySignalDate = date;
        populateSignalHistory(state);
    }
    size_t previousCount = state.strategySignalHistory.records().size();
    if (!state.strategySignalHistory.observe(state.activeStrategyId, state.dataMode,
                                             state.strategySignals)) {
        return;
    }
    populateSignalHistory(state);
    const auto& records = state.strategySignalHistory.records();
    if (state.dataMode == MarketDataMode::Live) {
        try {
            state.strategySignalStore.save(date, records);
        } catch (const std::exception& ex) {
            logger().write("WARN", std::string("Strategy signal save failed: ") + ex.what());
        }
    }
    for (size_t index = previousCount; index < records.size(); ++index) {
        const auto& record = records[index];
        logger().write("INFO", "Strategy signal recorded source=" +
                                   std::string(record.dataMode == MarketDataMode::Replay ? "replay" : "live") +
                                   " strategy=" + record.strategyId +
                                   " symbol=" + record.signal.symbol +
                                   " side=" + (record.signal.type == SignalType::Buy ? "BUY" : "SELL") +
                                   " price=" + std::to_string(record.signal.currentPrice) +
                                   " quantity=" + std::to_string(record.signal.suggestedQuantity) +
                                   " time=" + record.signal.time);
    }
}

void replaceReconstructedSignals(AppState& state,
                                 const std::vector<TradingSignal>& reconstructed) {
    std::vector<StrategySignalRecord> records = state.strategySignalHistory.records();
    records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& record) {
                      return record.dataMode == MarketDataMode::Live &&
                             record.strategyId == state.activeStrategyId;
                  }),
                  records.end());
    for (const auto& signal : reconstructed) {
        if (signal.type == SignalType::None) {
            continue;
        }
        records.push_back({MarketDataMode::Live, state.activeStrategyId, signal});
    }
    std::stable_sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        if (left.signal.time != right.signal.time) return left.signal.time < right.signal.time;
        if (left.signal.symbol != right.signal.symbol) return left.signal.symbol < right.signal.symbol;
        return static_cast<int>(left.signal.type) < static_cast<int>(right.signal.type);
    });
    records.erase(std::unique(records.begin(), records.end(), [](const auto& left, const auto& right) {
                      return left.strategyId == right.strategyId &&
                             left.dataMode == right.dataMode &&
                             left.signal.symbol == right.signal.symbol &&
                             left.signal.type == right.signal.type &&
                             left.signal.time == right.signal.time;
                  }),
                  records.end());
    state.strategySignalHistory.restore(std::move(records));
    state.strategySignalDate = currentDateToken();
    state.strategySignalStore.save(state.strategySignalDate,
                                   state.strategySignalHistory.records());
    populateSignalHistory(state);
}

void replaceReconstructedTrades(AppState& state,
                                const std::vector<TradeRecord>& reconstructed) {
    const std::string date = currentDateToken();
    state.strategyTrades.erase(
        std::remove_if(state.strategyTrades.begin(), state.strategyTrades.end(),
                       [&](const TradeRecord& trade) {
                           return trade.source == TradeSource::StrategySimulation &&
                                  trade.strategyId == state.activeStrategyId &&
                                  tradeDateToken(trade.time) == date;
                       }),
        state.strategyTrades.end());
    state.strategyTrades.insert(state.strategyTrades.end(), reconstructed.begin(),
                                reconstructed.end());
    state.strategyTradeStore.save(state.strategyTrades);
}

void clearStrategySignalHistory(AppState& state) {
    state.strategySignalHistory.reset();
    populateSignalHistory(state);
}

void evaluateReplayStrategies(AppState& state) {
    if (!state.replay.ready() || state.strategySymbols.empty()) {
        state.strategySignals.clear();
        if (state.detailView == DetailView::Strategy) {
            populateStrategySignals(state);
        }
        return;
    }
    std::vector<TradingSignal> signals;
    bool activeStrategyTraded = false;
    // 所有算法在同一回放分钟都执行，切换组合框时可直接比较完整的模拟收益。
    for (const auto& strategy : state.strategies) {
        auto algorithm = createTradingAlgorithm(strategy);
        for (const auto& symbol : state.strategySymbols) {
            try {
                ensureReplaySymbol(state, symbol);
                auto quote = state.replay.quote(symbol);
                if (!quote) {
                    if (strategy.id == state.activeStrategyId) {
                        TradingSignal waiting;
                        waiting.symbol = symbol;
                        waiting.name = symbolName(state, symbol);
                        waiting.algorithm = strategy.name;
                        waiting.message = "等待该标的开盘分钟数据";
                        waiting.time = state.replay.date() + " " + state.replay.currentTime();
                        signals.push_back(std::move(waiting));
                    }
                    continue;
                }
                state.quoteCache[symbol] = *quote;
                auto initialHolding = state.holdings.find(symbol);
                const Holding* initial = initialHolding == state.holdings.end()
                                             ? nullptr
                                             : &initialHolding->second;
                state.strategySimulator.ensureAccount(strategy, *quote, initial);
                Holding simulatedHolding = state.strategySimulator.holdingContext(strategy.id, symbol);
                const Holding* holdingContext = simulatedHolding.quantity > 0 ? &simulatedHolding : nullptr;
                TradingSignal signal = algorithm->evaluate(*quote, state.replay.dailyLines(symbol, strategy.technicalSwing.enableTechnicalSwingStrategy ? 250 : 80),
                                                           state.replay.minuteLines(symbol), holdingContext);
                SimulationExecution execution = state.strategySimulator.processSignal(
                    strategy, signal, *quote, initial);
                if (execution.executed && execution.trade) {
                    signal.message += "；" + execution.message;
                    logger().write("INFO", "Replay simulated trade strategy=" + strategy.id +
                                               " symbol=" + symbol +
                                               " side=" + (signal.type == SignalType::Buy ? "BUY" : "SELL") +
                                               " quantity=" + std::to_string(execution.trade->quantity) +
                                               " price=" + std::to_string(execution.trade->executionPrice) +
                                               " fees=" + std::to_string(execution.trade->fees) +
                                               " time=" + signal.time);
                    if (strategy.id == state.activeStrategyId) {
                        activeStrategyTraded = true;
                    }
                } else if (execution.signalTransition && !execution.message.empty()) {
                    signal.message += "；" + execution.message;
                }
                if (strategy.id == state.activeStrategyId) {
                    signals.push_back(std::move(signal));
                }
            } catch (const std::exception& ex) {
                if (strategy.id == state.activeStrategyId) {
                    TradingSignal failed;
                    failed.symbol = symbol;
                    failed.name = symbolName(state, symbol);
                    failed.algorithm = strategy.name;
                    failed.message = std::string("回放分析失败：") + ex.what();
                    failed.time = state.replay.date() + " " + state.replay.currentTime();
                    signals.push_back(std::move(failed));
                }
                logger().write("WARN", "Replay strategy failed strategy=" + strategy.id +
                                           " symbol=" + symbol + " error=" + ex.what());
            }
        }
    }
    state.strategySignals = std::move(signals);
    captureStrategySignals(state);
    if (state.detailView == DetailView::Strategy) {
        populateStrategySignals(state);
        if (state.strategyDetailsShowTrades) {
            populateTradeDetails(state);
        }
        state.hoveredTradeMarker.reset();
        InvalidateRect(state.kline, nullptr, FALSE);
    }
    if (activeStrategyTraded) {
        state.hoveredTradeMarker.reset();
        InvalidateRect(state.kline, nullptr, FALSE);
    }
}

}  // namespace

void restoreTodayStrategySignals(AppState& state) {
    try {
        const std::string date = currentDateToken();
        auto records = state.strategySignalStore.load(date);
        state.strategySignalHistory.restore(std::move(records));
        state.strategySignalDate = date;
        state.shownAlertKeys.clear();
        for (const auto& record : state.strategySignalHistory.records()) {
            state.shownAlertKeys.insert(date + "|" + record.signal.symbol + "|" +
                                        record.signal.algorithm + "|" +
                                        std::to_string(static_cast<int>(record.signal.type)));
        }
        populateSignalHistory(state);
        logger().write("INFO", "Strategy signals restored count=" +
                                   std::to_string(state.strategySignalHistory.records().size()));
    } catch (const std::exception& ex) {
        state.strategySignalHistory.reset();
        populateSignalHistory(state);
        logger().write("WARN", std::string("Strategy signal restore failed: ") + ex.what());
    }
}

void switchDetailView(AppState& state, DetailView view) {
    DetailView previousView = state.detailView;
    PageChartSelection& previousSelection = chartSelectionForView(state, previousView);
    previousSelection.symbol = state.currentSymbol;
    previousSelection.klt = state.currentKlt;
    // 回放推进会触发策略模拟；离开策略页时立即停止隐藏的播放定时器。
    if (view != DetailView::Strategy && state.replayPlaying) {
        setReplayPlaying(state, false);
    }
    bool chartPage = view == DetailView::Favorites || view == DetailView::Holdings ||
                     view == DetailView::Strategy;
    if (state.dataMode == MarketDataMode::Replay && state.replay.ready() &&
        view != previousView && chartPage) {
        if (view == DetailView::Strategy) {
            // 策略页从首个真实分钟重新开始，确保播放不会混入普通页面的收盘快照。
            state.replay.reset();
            state.strategySimulator.reset();
            state.strategySignals.clear();
            clearStrategySignalHistory(state);
        } else {
            // 行情总览和持仓管理是静态查看页，直接展示完整交易日数据。
            state.replay.advance(state.replay.totalMinutes());
        }
    }
    state.detailView = view;
    setWindowTextIfChanged(state.pageTitle, view == DetailView::Favorites ? L"行情总览" :
                                                 view == DetailView::Holdings ? L"持仓管理" :
                                                 view == DetailView::Strategy ? L"策略观察" :
                                                 view == DetailView::Screener ? L"策略选股" :
                                                 view == DetailView::Research ? L"算法研究" :
                                                                                L"交易分析");
    if (view == DetailView::Screener || view == DetailView::Research ||
        view == DetailView::TradeAnalysis) {
        updateToolbarVisibility(state);
        RECT client{};
        GetClientRect(state.window, &client);
        layout(state, client.right - client.left, client.bottom - client.top);
        InvalidateRect(state.window, nullptr, TRUE);
        InvalidateRect(GetDlgItem(state.window, IDC_VIEW_FAVORITES), nullptr, TRUE);
        InvalidateRect(GetDlgItem(state.window, IDC_VIEW_HOLDINGS), nullptr, TRUE);
        InvalidateRect(GetDlgItem(state.window, IDC_VIEW_STRATEGY), nullptr, TRUE);
        InvalidateRect(GetDlgItem(state.window, IDC_VIEW_SCREENER), nullptr, TRUE);
        InvalidateRect(GetDlgItem(state.window, IDC_VIEW_RESEARCH), nullptr, TRUE);
        InvalidateRect(GetDlgItem(state.window, IDC_VIEW_TRADE_ANALYSIS), nullptr, TRUE);
        return;
    }
    configureDetailColumns(state);
    configureSignalHistoryColumns(state);
    updateToolbarVisibility(state);

    // 每个页面只恢复自己的上次选择；失效的持仓/观察标的才回退到该页首项。
    PageChartSelection& targetSelection = chartSelectionForView(state, view);
    std::optional<std::string> preferredSymbol;
    if (view == DetailView::Favorites) {
        preferredSymbol = !targetSelection.symbol.empty()
                              ? std::optional<std::string>(targetSelection.symbol)
                              : std::optional<std::string>(majorIndexSymbols().front());
    } else if (view == DetailView::Holdings) {
        if (state.holdings.find(targetSelection.symbol) != state.holdings.end()) {
            preferredSymbol = targetSelection.symbol;
        } else if (!state.holdings.empty()) {
            preferredSymbol = state.holdings.begin()->first;
        }
    } else if (view == DetailView::Strategy) {
        if (state.strategySymbols.find(targetSelection.symbol) != state.strategySymbols.end()) {
            preferredSymbol = targetSelection.symbol;
        } else if (!state.strategySymbols.empty()) {
            preferredSymbol = *state.strategySymbols.begin();
        }
    }
    int preferredKlt = targetSelection.klt == 101 ? 101 : 1;
    if (preferredSymbol) {
        targetSelection.symbol = *preferredSymbol;
        targetSelection.klt = preferredKlt;
        SetWindowTextW(state.edit, utf8ToWide(*preferredSymbol).c_str());
        if (*preferredSymbol != state.currentSymbol || preferredKlt != state.currentKlt ||
            state.kLines.empty()) {
            try {
                loadKLine(state, *preferredSymbol, preferredKlt);
            } catch (const std::exception& ex) {
                state.currentSymbol = *preferredSymbol;
                state.currentKlt = preferredKlt;
                targetSelection.symbol = *preferredSymbol;
                targetSelection.klt = preferredKlt;
                state.kLines.clear();
                logger().write("WARN", std::string("View chart load failed: ") + ex.what());
            }
        }
    } else if (!preferredSymbol) {
        state.currentSymbol.clear();
        targetSelection.symbol.clear();
        targetSelection.klt = preferredKlt;
        state.kLines.clear();
        InvalidateRect(state.kline, nullptr, FALSE);
    }
    populateDetailView(state);
    if (state.dataMode == MarketDataMode::Replay && state.replay.ready()) {
        try {
            // 回放暂停时没有定时器，切页也要立即生成该页在当前分钟的快照。
            refreshQuoteLists(state);
            if (view == DetailView::Strategy) {
                evaluateReplayStrategies(state);
            }
        } catch (const std::exception& ex) {
            logger().write("WARN", std::string("Replay view refresh failed: ") + ex.what());
        }
    }
    if ((view == DetailView::Holdings || view == DetailView::Strategy) &&
        preferredSymbol) {
        SetWindowTextW(state.edit, utf8ToWide(*preferredSymbol).c_str());
        auto holding = state.holdings.find(*preferredSymbol);
        if (holding != state.holdings.end()) {
            long long quantity = holding->second.tradeBaseDate == currentDateToken()
                                     ? holding->second.tradeBaseQuantity
                                     : holding->second.quantity;
            double cost = holding->second.tradeBaseDate == currentDateToken()
                              ? holding->second.tradeBaseCost
                              : holding->second.cost;
            SetWindowTextW(state.quantityEdit, std::to_wstring(quantity).c_str());
            SetWindowTextW(state.costEdit, formatNumber(cost, 3).c_str());
        } else {
            SetWindowTextW(state.quantityEdit, L"");
            SetWindowTextW(state.costEdit, L"");
        }
    } else if (view == DetailView::Holdings || view == DetailView::Strategy) {
        SetWindowTextW(state.quantityEdit, L"");
        SetWindowTextW(state.costEdit, L"");
    }
    RECT client{};
    GetClientRect(state.window, &client);
    layout(state, client.right - client.left, client.bottom - client.top);
    InvalidateRect(state.window, nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_VIEW_FAVORITES), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_VIEW_HOLDINGS), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_VIEW_STRATEGY), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_VIEW_SCREENER), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_VIEW_RESEARCH), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_VIEW_TRADE_ANALYSIS), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_DAY_KLINE), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_MIN_KLINE), nullptr, TRUE);
    if (state.dataMode == MarketDataMode::Replay && state.replay.ready()) {
        setStatus(state, replayProgressText(state) + L"  已切换页面。");
    } else if (view == DetailView::Favorites) {
        setStatus(state, L"已切换到自选行情。");
    } else if (view == DetailView::Holdings) {
        setStatus(state, L"已切换到持仓；输入代码、数量和成本可新增或修改持仓。");
    } else if (state.strategySymbols.empty()) {
        setStatus(state, L"请从自选列表中选择标的并加入策略观察。");
    } else {
        setStatus(state, L"选择观察标的后可修改策略使用的底仓数量和成本。");
    }
}

bool sameKLines(const std::vector<KLine>& left, const std::vector<KLine>& right) {
    // 精确比较行情源字段；完全相同时跳过图表重绘，减少定时刷新抖动。
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        const KLine& first = left[index];
        const KLine& second = right[index];
        if (first.date != second.date || first.open != second.open || first.close != second.close ||
            first.high != second.high || first.low != second.low || first.volume != second.volume) {
            return false;
        }
    }
    return true;
}

void loadKLine(AppState& state, const std::string& symbol, int klt) {
    // klt=-1 表示沿用当前周期，显式传入 1/101 时切换分时或日线。
    int targetKlt = klt > 0 ? klt : state.currentKlt;
    bool viewChanged = symbol != state.currentSymbol || targetKlt != state.currentKlt;
    std::vector<KLine> fresh;
    if (state.dataMode == MarketDataMode::Replay) {
        if (!state.replay.ready()) {
            throw std::runtime_error("请先选择交易日并加载回放行情。");
        }
        ensureReplaySymbol(state, symbol);
        fresh = targetKlt == 1 ? state.replay.minuteLines(symbol)
                               : state.replay.dailyLines(symbol, 80);
    } else {
        if (targetKlt == 1 && !viewChanged && !state.kLines.empty()) {
            fresh = state.kLines;
            auto incremental = state.provider.fetchKLines(
                symbol, targetKlt, INCREMENTAL_MINUTE_TAIL_COUNT);
            mergeIncrementalMinuteLines(fresh, incremental,
                                        FULL_TRADING_MINUTE_COUNT);
        } else {
            fresh = state.provider.fetchKLines(
                symbol, targetKlt,
                targetKlt == 1 ? FULL_TRADING_MINUTE_COUNT : 80);
        }
    }
    if (!viewChanged && sameKLines(state.kLines, fresh)) {
        return;
    }
    state.currentSymbol = symbol;
    state.currentKlt = targetKlt;
    PageChartSelection& pageSelection = chartSelectionForView(state, state.detailView);
    pageSelection.symbol = symbol;
    pageSelection.klt = targetKlt;
    state.kLines = std::move(fresh);
    if (viewChanged) {
        state.hoveredTradeMarker.reset();
        state.hoveredCandleMarker.reset();
    }
    InvalidateRect(state.kline, nullptr, FALSE);
}

void refreshQuoteLists(AppState& state) {
    // 只请求当前页面需要的标的；主要指数仅在行情总览页面加入请求集合。
    std::set<std::string> requested = requestedSymbolsForView(state);
    std::vector<std::string> symbols(requested.begin(), requested.end());
    std::vector<Quote> quotes;
    if (state.dataMode == MarketDataMode::Replay) {
        if (!state.replay.ready()) {
            throw std::runtime_error("请先选择交易日并加载回放行情。");
        }
        // 每个回放时点重新生成快照，重置进度后不会残留未来分钟的价格。
        state.quoteCache.clear();
        std::string firstError;
        for (const auto& symbol : symbols) {
            try {
                ensureReplaySymbol(state, symbol);
            } catch (const std::exception& ex) {
                if (firstError.empty()) {
                    firstError = ex.what();
                }
                logger().write("WARN", "Replay quote load failed symbol=" + symbol + " error=" + ex.what());
            }
        }
        quotes = state.replay.quotes(symbols);
        if (quotes.empty() && !symbols.empty() && !firstError.empty()) {
            throw std::runtime_error(firstError);
        }
    } else {
        quotes = state.provider.fetchQuotes(symbols);
    }
    for (const auto& quote : quotes) {
        state.quoteCache[quote.symbol] = quote;
        if (!quote.name.empty()) {
            state.symbolNames[quote.symbol] = quote.name;
        }
    }

    if (state.detailView == DetailView::Favorites) {
        state.updatingLists = true;
        populateQuotes(state.indexList, cachedQuotesInOrder(state, majorIndexSymbols()));
        selectSymbolInList(state.indexList, state.currentSymbol);
        state.updatingLists = false;
    }
    populateDetailView(state);
}

void refreshDashboard(AppState& state, bool refreshChart) {
    // 列表和图表相互独立刷新：一项失败不阻止另一项使用最新可用数据。
    std::string firstError;
    auto rememberError = [&](const std::exception& ex) {
        if (firstError.empty()) {
            firstError = ex.what();
        }
    };

    try {
        refreshQuoteLists(state);
    } catch (const std::exception& ex) {
        rememberError(ex);
    }
    if (refreshChart && !state.currentSymbol.empty()) {
        try {
            loadKLine(state, state.currentSymbol);
        } catch (const std::exception& ex) {
            rememberError(ex);
        }
    }

    if (!firstError.empty()) {
        throw std::runtime_error(firstError);
    }
    if (state.dataMode == MarketDataMode::Replay) {
        setStatus(state, replayProgressText(state));
    } else {
        setStatus(state, state.detailView == DetailView::Favorites ? L"行情总览已刷新。" :
                         state.detailView == DetailView::Holdings ? L"持仓行情已刷新。" :
                                                                    L"策略观察行情已刷新。");
    }
}

void searchSymbol(AppState& state) {
    auto symbol = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
    if (!symbol) {
        MessageBoxW(state.window, L"代码格式不正确，例如 600000、sh000001、sz399006 或 hs300。", L"输入错误", MB_ICONWARNING);
        return;
    }
    std::vector<Quote> quotes;
    if (state.dataMode == MarketDataMode::Replay) {
        if (!state.replay.ready()) {
            throw std::runtime_error("请先加载回放行情，再查询回放标的。");
        }
        ensureReplaySymbol(state, *symbol);
        quotes = state.replay.quotes({*symbol});
    } else {
        quotes = state.provider.fetchQuotes({*symbol});
    }
    if (quotes.empty()) {
        throw std::runtime_error("未获取到该证券或指数的实时行情。");
    }
    state.updatingLists = true;
    populateQuotes(state.list, quotes);
    selectSymbolInList(state.list, *symbol);
    state.updatingLists = false;
    ListView_SetItemState(state.indexList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    loadKLine(state, *symbol);
    setWindowTextIfChanged(state.favoritesLabel, L"查询结果");
    setStatus(state, L"已查询 " + utf8ToWide(*symbol));
}

void addFavorite(AppState& state) {
    auto symbol = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
    if (!symbol) {
        MessageBoxW(state.window, L"证券代码格式不正确。", L"输入错误", MB_ICONWARNING);
        return;
    }
    state.favorites.insert(*symbol);
    state.store.save(state.favorites);
    refreshQuoteLists(state);
    selectSymbolInList(state.list, *symbol);
    logger().write("INFO", "Favorite added symbol=" + *symbol);
    setStatus(state, L"已加入自选 " + utf8ToWide(*symbol));
}

std::optional<std::string> selectedSymbol(AppState& state) {
    auto symbol = selectedSymbolFromList(state.list);
    return symbol ? symbol : selectedSymbolFromList(state.indexList);
}

void deleteFavorite(AppState& state) {
    auto symbol = selectedSymbolFromList(state.list);
    if (!symbol) {
        symbol = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
    }
    if (!symbol) {
        MessageBoxW(state.window, L"请先在列表中选中标的，或在输入框填写代码。", L"提示", MB_ICONINFORMATION);
        return;
    }
    state.favorites.erase(*symbol);
    state.strategySymbols.erase(*symbol);
    state.store.save(state.favorites);
    state.strategyStore.save(state.strategySymbols);
    refreshQuoteLists(state);
    logger().write("INFO", "Favorite removed symbol=" + *symbol);
    setStatus(state, L"已删除自选 " + utf8ToWide(*symbol));
}

bool saveHoldingValues(AppState& state, const std::string& rawSymbol,
                       long long quantity, double cost, HWND messageOwner) {
    HWND owner = messageOwner ? messageOwner : state.window;
    auto symbol = normalizeSymbol(rawSymbol);
    if (!symbol) {
        MessageBoxW(owner, L"请输入有效的证券代码。", L"输入错误", MB_ICONWARNING);
        return false;
    }
    if (std::find(majorIndexSymbols().begin(), majorIndexSymbols().end(), *symbol) != majorIndexSymbols().end()) {
        MessageBoxW(owner, L"指数不能加入持仓，请输入股票或 ETF 代码。", L"输入错误", MB_ICONWARNING);
        return false;
    }
    if (quantity <= 0 || !std::isfinite(cost) || cost <= 0.0) {
        MessageBoxW(owner, L"持仓数量和成本必须大于 0。", L"输入错误", MB_ICONWARNING);
        return false;
    }
    cost = std::round(cost * 1000.0) / 1000.0;

    auto existing = state.holdings.find(*symbol);
    Holding holding = existing == state.holdings.end() ? Holding{} : existing->second;
    holding.symbol = *symbol;
    holding.tradeBaseDate = currentDateToken();
    holding.tradeBaseQuantity = quantity;
    holding.tradeBaseCost = cost;
    std::string ledgerError;
    if (!recalculateHoldingFromTrades(holding, state.holdingTrades,
                                      currentDateToken(), &ledgerError)) {
        MessageBoxW(owner, utf8ToWide(ledgerError).c_str(), L"持仓重算失败",
                    MB_ICONWARNING);
        return false;
    }
    state.holdings[*symbol] = holding;
    state.holdingStore.save(state.holdings);
    SetWindowTextW(state.edit, utf8ToWide(*symbol).c_str());
    SetWindowTextW(state.quantityEdit, std::to_wstring(quantity).c_str());
    SetWindowTextW(state.costEdit, formatNumber(cost, 3).c_str());
    refreshQuoteLists(state);
    selectSymbolInList(state.list, *symbol);
    updateHoldingSummary(state);
    logger().write("INFO", "Holding saved symbol=" + *symbol + " quantity=" + std::to_string(quantity) +
                               " cost=" + std::to_string(cost));
    if (state.detailView == DetailView::Strategy) {
        state.strategySignalRebuildDate.clear();
        startStrategyScan(state);
        setStatus(state, L"策略底仓已保存 " + utf8ToWide(*symbol));
    } else {
        setStatus(state, L"持仓已保存 " + utf8ToWide(*symbol));
    }
    return true;
}

void saveHolding(AppState& state) {
    long long quantity = 0;
    double cost = 0.0;
    try {
        std::wstring quantityText = getEditText(state.quantityEdit);
        std::wstring costText = getEditText(state.costEdit);
        size_t quantityEnd = 0;
        size_t costEnd = 0;
        quantity = std::stoll(quantityText, &quantityEnd);
        cost = std::stod(costText, &costEnd);
        if (quantityEnd != quantityText.size() || costEnd != costText.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (const std::exception&) {
        MessageBoxW(state.window, L"持仓数量和成本必须是有效数字。", L"输入错误", MB_ICONWARNING);
        return;
    }
    saveHoldingValues(state, wideToUtf8(getEditText(state.edit)), quantity, cost, state.window);
}

void deleteHolding(AppState& state) {
    auto symbol = selectedSymbolFromList(state.list);
    if (!symbol) {
        symbol = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
    }
    if (!symbol || state.holdings.erase(*symbol) == 0) {
        MessageBoxW(state.window, L"请先选择要删除的持仓。", L"提示", MB_ICONINFORMATION);
        return;
    }
    state.holdingTrades.erase(
        std::remove_if(state.holdingTrades.begin(), state.holdingTrades.end(),
                       [&](const TradeRecord& trade) { return trade.symbol == *symbol; }),
        state.holdingTrades.end());
    state.holdingStore.save(state.holdings);
    state.holdingTradeStore.save(state.holdingTrades);
    populateDetailView(state);
    logger().write("INFO", "Holding removed symbol=" + *symbol);
    setStatus(state, L"持仓已删除 " + utf8ToWide(*symbol));
}

void toggleStrategySymbol(AppState& state) {
    auto symbol = selectedSymbolFromList(state.list);
    if (!symbol) {
        symbol = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
    }
    if (!symbol || state.favorites.find(*symbol) == state.favorites.end()) {
        MessageBoxW(state.window, L"策略观察只能从自选标的中添加，请先加入自选。", L"提示", MB_ICONINFORMATION);
        return;
    }
    auto existing = state.strategySymbols.find(*symbol);
    if (existing != state.strategySymbols.end()) {
        state.strategySymbols.erase(existing);
        setStatus(state, L"已取消策略观察 " + utf8ToWide(*symbol));
        logger().write("INFO", "Strategy symbol removed symbol=" + *symbol);
    } else {
        constexpr size_t maxStrategySymbols = 10;
        if (state.strategySymbols.size() >= maxStrategySymbols) {
            MessageBoxW(state.window, L"策略观察最多支持 10 个标的，请先移除不需要的标的。", L"数量限制", MB_ICONINFORMATION);
            return;
        }
        state.strategySymbols.insert(*symbol);
        setStatus(state, L"已加入策略观察 " + utf8ToWide(*symbol));
        logger().write("INFO", "Strategy symbol added symbol=" + *symbol);
    }
    state.strategyStore.save(state.strategySymbols);
}

void deleteStrategySymbol(AppState& state) {
    auto symbol = selectedSymbolFromList(state.list);
    if (!symbol || state.strategySymbols.erase(*symbol) == 0) {
        MessageBoxW(state.window, L"请先选择要移出的策略标的。", L"提示", MB_ICONINFORMATION);
        return;
    }
    state.strategyStore.save(state.strategySymbols);
    state.strategySignals.erase(
        std::remove_if(state.strategySignals.begin(), state.strategySignals.end(),
                       [&](const TradingSignal& signal) { return signal.symbol == *symbol; }),
        state.strategySignals.end());
    populateDetailView(state);
    logger().write("INFO", "Strategy symbol removed symbol=" + *symbol);
    setStatus(state, L"已移出策略观察 " + utf8ToWide(*symbol));
}

void toggleTechnicalSwing(AppState& state) {
    if (state.strategyScanRunning) return;
    auto active = std::find_if(state.strategies.begin(), state.strategies.end(),
        [&](const StrategyConfig& c) { return c.id == state.activeStrategyId; });
    if (active == state.strategies.end()) return;
    bool enabled = !active->technicalSwing.enableTechnicalSwingStrategy;
    auto updated = state.strategies;
    updated[static_cast<size_t>(active - state.strategies.begin())].technicalSwing.enableTechnicalSwingStrategy = enabled;
    state.strategyConfigStore.save(updated);
    state.strategies = std::move(updated);
    for (auto& signal : state.strategySignals) signal.technicalSwingResult.reset();
    populateTechnicalSwingPanel(state);
    InvalidateRect(state.kline, nullptr, FALSE);
    if (enabled) startStrategyScan(state);
}

void startStrategyScan(AppState& state) {
    if (state.dataMode == MarketDataMode::Replay) {
        evaluateReplayStrategies(state);
        return;
    }
    if (state.strategyScanRunning) {
        return;
    }
    if (state.strategySymbols.empty()) {
        if (state.detailView == DetailView::Strategy) {
            setStatus(state, L"请从自选列表中选择标的并加入策略观察。");
        }
        return;
    }
    auto active = std::find_if(state.strategies.begin(), state.strategies.end(),
                               [&](const StrategyConfig& config) { return config.id == state.activeStrategyId; });
    if (active == state.strategies.end()) {
        return;
    }
    StrategyConfig strategy = *active;
    const std::string scanDate = currentDateToken();
    const bool reconstructToday = strategy.kind == StrategyKind::T0Intraday &&
                                  state.strategySignalRebuildDate != scanDate;
    if (reconstructToday) {
        // 在线程启动前占位，避免 5 秒定时器并发启动多个整日重放任务。
        state.strategySignalRebuildDate = scanDate;
    }

    state.strategyScanRunning = true;
    if (state.detailView == DetailView::Strategy) {
        populateStrategySignals(state);
    }
    // 在线程启动前复制全部输入，后台任务不直接访问可能变化的 AppState 容器。
    std::vector<std::string> symbols(state.strategySymbols.begin(), state.strategySymbols.end());
    std::map<std::string, Holding> holdings = state.holdings;
    std::shared_ptr<StrategyMailbox> mailbox = state.strategyMailbox;
    std::shared_ptr<StrategyHistoryCache> historyCache = state.strategyHistoryCache;
    std::filesystem::path replayCacheDirectory = applicationStorageDirectory() / "replay_cache";
    bool fastT0Scan = strategy.kind == StrategyKind::T0Intraday;
    if (!fastT0Scan) {
        logger().write("INFO", "Strategy scan started strategy=" + strategy.id +
                                   " symbols=" + std::to_string(symbols.size()));
    }

    auto scanTask = [symbols = std::move(symbols), holdings = std::move(holdings),
                      mailbox = std::move(mailbox), historyCache = std::move(historyCache),
                      strategy = std::move(strategy), fastT0Scan, reconstructToday,
                      scanDate, replayCacheDirectory = std::move(replayCacheDirectory)]() {
        StrategyScanResult result;
        result.strategyId = strategy.id;
        result.reconstructionAttempted = reconstructToday;
        int failures = 0;
        try {
            // 每个扫描线程使用独立 provider，避免跨线程共享 WinHTTP session。
            QuoteProvider provider;
            std::map<std::string, Quote> quotesBySymbol;
            std::map<std::string, std::vector<KLine>> dailyForReconstruction;
            std::map<std::string, std::vector<KLine>> minuteForReconstruction;
            for (const auto& quote : provider.fetchQuotes(symbols)) {
                quotesBySymbol[quote.symbol] = quote;
            }
            auto algorithm = createTradingAlgorithm(strategy);
            for (const auto& symbol : symbols) {
                std::vector<KLine> daily;
                try {
                    auto quote = quotesBySymbol.find(symbol);
                    if (quote == quotesBySymbol.end()) {
                        throw std::runtime_error("未获取到实时行情");
                    }
                    // T0 每 5 秒扫描实时价格，但历史 K 线按较低频率刷新，降低网络请求和服务端压力。
                    const auto now = std::chrono::steady_clock::now();
                    std::vector<KLine> minute;
                    bool dailyFresh = false;
                    bool minuteFresh = false;
                    {
                        std::lock_guard<std::mutex> lock(historyCache->mutex);
                        auto cached = historyCache->entries.find(symbol);
                        if (cached != historyCache->entries.end()) {
                            daily = cached->second.daily;
                            minute = cached->second.minute;
                            dailyFresh = !daily.empty() &&
                                         (!strategy.technicalSwing.enableTechnicalSwingStrategy ||
                                          cached->second.dailyRequestedLimit >= 250) &&
                                         now - cached->second.dailyUpdated < STRATEGY_DAILY_CACHE_LIFETIME;
                            minuteFresh = !minute.empty() &&
                                          now - cached->second.minuteUpdated < STRATEGY_MINUTE_CACHE_LIFETIME;
                        }
                    }

                    if (!dailyFresh) {
                        try {
                            std::vector<KLine> freshDaily = provider.fetchKLines(symbol, 101, strategy.technicalSwing.enableTechnicalSwingStrategy ? 250 : 80);
                            if (!freshDaily.empty()) {
                                daily = std::move(freshDaily);
                                std::lock_guard<std::mutex> lock(historyCache->mutex);
                                auto& cached = historyCache->entries[symbol];
                                cached.daily = daily;
                                cached.dailyUpdated = now;
                                cached.dailyRequestedLimit = strategy.technicalSwing.enableTechnicalSwingStrategy ? 250 : 80;
                            }
                        } catch (const std::exception& ex) {
                            if (daily.empty()) {
                                throw;
                            }
                            logger().write("WARN", "Using stale daily strategy data symbol=" + symbol +
                                                       " error=" + ex.what());
                        }
                    }

                    if (!minuteFresh) {
                        try {
                            const int requestCount = minute.empty()
                                                         ? FULL_TRADING_MINUTE_COUNT
                                                         : INCREMENTAL_MINUTE_TAIL_COUNT;
                            std::vector<KLine> freshMinute =
                                provider.fetchKLines(symbol, 1, requestCount);
                            if (!freshMinute.empty()) {
                                if (minute.empty()) {
                                    minute = std::move(freshMinute);
                                } else {
                                    mergeIncrementalMinuteLines(
                                        minute, freshMinute,
                                        FULL_TRADING_MINUTE_COUNT);
                                }
                                std::lock_guard<std::mutex> lock(historyCache->mutex);
                                auto& cached = historyCache->entries[symbol];
                                cached.minute = minute;
                                cached.minuteUpdated = now;
                            }
                        } catch (const std::exception& ex) {
                            if (minute.empty()) {
                                throw;
                            }
                            logger().write("WARN", "Using stale minute strategy data symbol=" + symbol +
                                                       " error=" + ex.what());
                        }
                    }

                    if (daily.empty() || minute.empty()) {
                        throw std::runtime_error("历史行情为空");
                    }
                    if (reconstructToday) {
                        dailyForReconstruction[symbol] = daily;
                        minuteForReconstruction[symbol] = minute;
                    }
                    auto holding = holdings.find(symbol);
                    const Holding* holdingContext = holding != holdings.end() ? &holding->second : nullptr;
                    if (!strategy.technicalSwing.enableTechnicalSwingStrategy && daily.size() > 80)
                        daily.erase(daily.begin(), daily.end() - 80);
                    TradingSignal signal = algorithm->evaluate(quote->second, daily, minute, holdingContext);
                    result.signals.push_back(std::move(signal));
                } catch (const std::exception& ex) {
                    // 单个标的失败只生成失败占位结果，不中断其他标的分析。
                    ++failures;
                    TradingSignal signal;
                    signal.symbol = symbol;
                    signal.algorithm = strategy.name;
                    signal.message = std::string("分析失败：") + ex.what();
                    // 日线分析不依赖分钟数据；原算法失败时仍可显示已有日线因子。
                    if (strategy.technicalSwing.enableTechnicalSwingStrategy && !daily.empty())
                        signal.technicalSwingResult = std::make_shared<TechnicalSwingResult>(
                            TechnicalSwingStrategy(strategy.technicalSwing).analyze(daily, symbol));
                    result.signals.push_back(std::move(signal));
                    logger().write("WARN", "Strategy scan failed symbol=" + symbol + " error=" + ex.what());
                }
            }
            if (reconstructToday) {
                try {
                    ReplayEngine replay(replayCacheDirectory);
                    replay.begin(isoDateToken(scanDate));
                    for (const auto& symbol : symbols) {
                        auto daily = dailyForReconstruction.find(symbol);
                        auto minute = minuteForReconstruction.find(symbol);
                        if (daily == dailyForReconstruction.end() ||
                            minute == minuteForReconstruction.end()) {
                            continue;
                        }
                        auto quote = quotesBySymbol.find(symbol);
                        std::string name = quote == quotesBySymbol.end()
                                               ? symbol
                                               : quote->second.name;
                        replay.installSymbolData(symbol, name, minute->second, daily->second);
                    }
                    if (!replay.ready()) {
                        throw std::runtime_error("没有可用于当日重建的分钟行情");
                    }
                    auto openingHoldings = makeOpeningHoldings(holdings, scanDate);
                    auto reconstruction = runStrategyDayBacktest(
                        strategy, replay, std::set<std::string>(symbols.begin(), symbols.end()),
                        openingHoldings);
                    result.reconstructedSignals =
                        std::move(reconstruction.signalTransitions);
                    for (const auto& trade : reconstruction.simulator.trades()) {
                        TradeRecord record;
                        record.id = "simulation|" + strategy.id + "|" + trade.symbol + "|" +
                                    trade.time + "|" +
                                    std::to_string(static_cast<int>(trade.side));
                        record.strategyId = strategy.id;
                        record.symbol = trade.symbol;
                        record.time = trade.time;
                        record.side = trade.side;
                        record.quantity = trade.quantity;
                        record.price = trade.executionPrice;
                        // 模拟器只返回合计费用，统一放入佣金列并明确标记为模拟来源。
                        record.commission = trade.fees;
                        record.source = TradeSource::StrategySimulation;
                        result.reconstructedTrades.push_back(std::move(record));
                    }
                    logger().write(
                        "INFO", "Strategy signals reconstructed date=" + scanDate +
                                    " strategy=" + strategy.id +
                                    " signals=" +
                                    std::to_string(result.reconstructedSignals.size()) +
                                    " trades=" +
                                    std::to_string(reconstruction.simulator.trades().size()));
                } catch (const std::exception& ex) {
                    result.reconstructionError = ex.what();
                    logger().write("WARN", "Strategy signal reconstruction failed date=" +
                                               scanDate + " error=" + ex.what());
                }
            }
            if (failures > 0) {
                result.error = std::to_string(failures) + " 个标的暂时分析失败，将在下次扫描时重试。";
            }
        } catch (const std::exception& ex) {
            result.error = std::string("策略扫描失败：") + ex.what();
            logger().write("ERROR", result.error);
        }

        if (!fastT0Scan) {
            logger().write("INFO", "Strategy scan finished results=" + std::to_string(result.signals.size()) +
                                       " failures=" + std::to_string(failures));
        }

        // 后台线程只写邮箱并投递消息，控件更新仍由 UI 线程完成。
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->pending = std::move(result);
        }
        HWND window = mailbox->window.load();
        if (mailbox->alive.load() && window) {
            PostMessageW(window, WM_APP_STRATEGY_RESULT, 0, 0);
        }
    };
    try {
        std::thread worker(std::move(scanTask));
        worker.detach();
    } catch (...) {
        state.strategyScanRunning = false;
        if (state.detailView == DetailView::Strategy) {
            populateStrategySignals(state);
        }
        throw;
    }
}

void handleStrategyResult(AppState& state) {
    std::optional<StrategyScanResult> pending;
    {
        std::lock_guard<std::mutex> lock(state.strategyMailbox->mutex);
        pending = std::move(state.strategyMailbox->pending);
        state.strategyMailbox->pending.reset();
    }
    if (!pending) {
        return;
    }
    state.strategyScanRunning = false;
    // 模式切换后到达的实盘后台结果不能覆盖当前回放时点。
    if (state.dataMode == MarketDataMode::Replay) {
        return;
    }
    // 扫描期间用户可能切换策略；旧结果丢弃并立即按新策略重扫。
    if (pending->strategyId != state.activeStrategyId) {
        startStrategyScan(state);
        return;
    }
    if (pending->reconstructionAttempted) {
        if (!pending->reconstructionError.empty()) {
            // 网络或历史数据暂时不可用时允许下一轮 5 秒扫描再次尝试。
            state.strategySignalRebuildDate.clear();
            if (pending->error.empty()) {
                pending->error = "当日信号暂未重建：" + pending->reconstructionError;
            }
        } else {
            try {
                replaceReconstructedSignals(state, pending->reconstructedSignals);
                replaceReconstructedTrades(state, pending->reconstructedTrades);
            } catch (const std::exception& ex) {
                state.strategySignalRebuildDate.clear();
                logger().write("WARN", std::string("Reconstructed signal save failed: ") + ex.what());
                if (pending->error.empty()) {
                    pending->error = std::string("当日信号保存失败：") + ex.what();
                }
            }
        }
    }
    state.strategySignals = std::move(pending->signals);
    captureStrategySignals(state);
    if (state.detailView == DetailView::Strategy) {
        populateStrategySignals(state);
    }
    if (state.detailView == DetailView::Strategy) {
        if (!pending->error.empty()) {
            setStatus(state, utf8ToWide(pending->error));
        } else {
            setStatus(state, L"策略分析已完成。技术信号仅供参考。");
        }
    }

    std::wstring alerts;
    std::string date = currentDateToken();
    for (const auto& signal : state.strategySignals) {
        if (signal.type == SignalType::None || state.strategySymbols.find(signal.symbol) == state.strategySymbols.end()) {
            continue;
        }
        // 同一交易日、标的、算法和方向只弹窗一次，避免每分钟重复提示。
        std::string key = date + "|" + signal.symbol + "|" + signal.algorithm + "|" +
                          std::to_string(static_cast<int>(signal.type));
        if (!state.shownAlertKeys.insert(key).second) {
            continue;
        }
        int precision = isFundLikeSymbol(signal.symbol) ? 3 : 2;
        alerts += utf8ToWide(signal.symbol + " " + signal.name) + L"  " + signalTypeText(signal.type) + L"\n";
        alerts += L"当前 " + formatNumber(signal.currentPrice, precision) + L"，参考窗口 " +
                  formatNumber(signal.windowLow, precision) + L" - " +
                  formatNumber(signal.windowHigh, precision);
        if (signal.suggestedQuantity > 0) {
            alerts += L"，建议数量 " + std::to_wstring(signal.suggestedQuantity);
        }
        alerts += L"\n" + utf8ToWide(signal.message) + L"\n\n";
    }
    if (!alerts.empty()) {
        std::wstring message = L"检测到以下技术信号：\n\n" + alerts +
                               L"提示：算法结果仅供行情研究参考，不构成投资建议。";
        MessageBoxW(state.window, message.c_str(), L"策略信号", MB_OK | MB_ICONINFORMATION);
    }
}

void selectDataMode(AppState& state) {
    int selected = static_cast<int>(SendMessageW(state.dataModeCombo, CB_GETCURSEL, 0, 0));
    MarketDataMode mode = selected == 1 ? MarketDataMode::Replay : MarketDataMode::Live;
    if (mode == state.dataMode) {
        return;
    }
    setReplayPlaying(state, false);
    state.dataMode = mode;
    state.quoteCache.clear();
    state.kLines.clear();
    state.strategySignals.clear();
    state.strategySimulator.reset();
    clearStrategySignalHistory(state);
    updateToolbarVisibility(state);
    RECT client{};
    GetClientRect(state.window, &client);
    layout(state, client.right - client.left, client.bottom - client.top);

    if (mode == MarketDataMode::Replay) {
        state.replay.clear();
        populateQuotes(state.indexList, {});
        populateDetailView(state);
        InvalidateRect(state.kline, nullptr, FALSE);
        setStatus(state, L"已切换到行情回放，请填写交易日并点击加载。");
        logger().write("INFO", "Market data mode changed to replay");
        return;
    }

    logger().write("INFO", "Market data mode changed to live");
    restoreTodayStrategySignals(state);
    refreshDashboard(state, true);
    startStrategyScan(state);
    setStatus(state, L"已恢复实盘行情。");
}

void loadReplaySession(AppState& state) {
    if (state.dataMode != MarketDataMode::Replay) {
        throw std::runtime_error("请先将数据源切换为行情回放。");
    }
    std::string date = trim(wideToUtf8(getEditText(state.replayDateEdit)));
    state.replay.begin(date);
    state.currentKlt = 1;
    chartSelectionForView(state, state.detailView).klt = 1;
    state.quoteCache.clear();
    state.strategySignals.clear();
    state.strategySimulator.reset();
    clearStrategySignalHistory(state);
    setReplayPlaying(state, false);
    setStatus(state, L"正在加载真实历史行情，请稍候...");
    UpdateWindow(state.window);

    std::set<std::string> symbols = requestedSymbolsForView(state);
    if (!state.currentSymbol.empty()) {
        symbols.insert(state.currentSymbol);
    }
    std::string firstError;
    int loaded = 0;
    for (const auto& symbol : symbols) {
        try {
            ensureReplaySymbol(state, symbol);
            ++loaded;
        } catch (const std::exception& ex) {
            if (firstError.empty()) {
                firstError = ex.what();
            }
            logger().write("WARN", "Replay session skipped symbol=" + symbol + " error=" + ex.what());
        }
    }
    if (loaded == 0) {
        state.replay.clear();
        throw std::runtime_error(firstError.empty() ? "该交易日没有可回放的真实行情。" : firstError);
    }
    if (state.detailView == DetailView::Strategy) {
        state.replay.reset();
    } else {
        // 普通页面没有播放控件，加载后直接定位到收盘，显示全天走势和全部标的。
        state.replay.advance(state.replay.totalMinutes());
    }
    if (state.currentSymbol.empty() || !state.replay.hasSymbol(state.currentSymbol)) {
        auto replacement = std::find_if(symbols.begin(), symbols.end(), [&](const std::string& symbol) {
            return state.replay.hasSymbol(symbol);
        });
        if (replacement != symbols.end()) {
            state.currentSymbol = *replacement;
            chartSelectionForView(state, state.detailView).symbol = *replacement;
            SetWindowTextW(state.edit, utf8ToWide(*replacement).c_str());
        }
    }
    refreshDashboard(state, true);
    if (state.detailView == DetailView::Strategy) {
        evaluateReplayStrategies(state);
    }
    InvalidateRect(GetDlgItem(state.window, IDC_DAY_KLINE), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_MIN_KLINE), nullptr, TRUE);
    setStatus(state, state.detailView == DetailView::Strategy
                         ? L"真实历史行情已加载，" + replayProgressText(state) +
                               L"。点击播放或单步验证算法。"
                         : L"真实历史行情已加载，" + replayProgressText(state) +
                               L"，已显示完整交易日快照。");
    logger().write("INFO", "Replay session loaded date=" + date +
                               " symbols=" + std::to_string(loaded));
}

void toggleReplayPlayback(AppState& state) {
    if (state.detailView != DetailView::Strategy) {
        throw std::runtime_error("播放功能仅在策略观察页面可用。");
    }
    if (state.dataMode != MarketDataMode::Replay || !state.replay.ready()) {
        throw std::runtime_error("请先选择交易日并加载回放行情。");
    }
    if (!state.replayPlaying && state.replay.finished()) {
        resetReplay(state);
    }
    setReplayPlaying(state, !state.replayPlaying);
    setStatus(state, replayProgressText(state) + (state.replayPlaying ? L"  正在播放" : L"  已暂停"));
}

void advanceReplay(AppState& state) {
    if (state.detailView != DetailView::Strategy) {
        setReplayPlaying(state, false);
        return;
    }
    if (state.dataMode != MarketDataMode::Replay || !state.replay.ready()) {
        setReplayPlaying(state, false);
        throw std::runtime_error("请先选择交易日并加载回放行情。");
    }
    bool moved = state.replay.advance();
    refreshDashboard(state, true);
    // 新信号只更新明细、模拟成交和图表标记，不改变用户控制的播放状态。
    evaluateReplayStrategies(state);
    if (!moved || state.replay.finished()) {
        setReplayPlaying(state, false);
        setStatus(state, replayProgressText(state) + L"  回放已结束。");
    } else {
        setStatus(state, replayProgressText(state) +
                         (state.replayPlaying ? L"  正在播放" : L"  单步完成"));
    }
}

void resetReplay(AppState& state) {
    if (state.detailView != DetailView::Strategy) {
        throw std::runtime_error("重置功能仅在策略观察页面可用。");
    }
    if (state.dataMode != MarketDataMode::Replay || !state.replay.ready()) {
        throw std::runtime_error("当前没有已加载的回放行情。");
    }
    setReplayPlaying(state, false);
    state.replay.reset();
    state.strategySimulator.reset();
    state.strategySignals.clear();
    clearStrategySignalHistory(state);
    refreshDashboard(state, true);
    evaluateReplayStrategies(state);
    setStatus(state, replayProgressText(state) + L"  已重置。");
}

void switchKLine(AppState& state, int klt) {
    auto symbol = selectedSymbol(state);
    if (!symbol) {
        symbol = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
    }
    if (!symbol) {
        MessageBoxW(state.window, L"请先查询或选择一个标的。", L"提示", MB_ICONINFORMATION);
        return;
    }
    loadKLine(state, *symbol, klt);
    InvalidateRect(GetDlgItem(state.window, IDC_DAY_KLINE), nullptr, TRUE);
    InvalidateRect(GetDlgItem(state.window, IDC_MIN_KLINE), nullptr, TRUE);
    setStatus(state, klt == 1 ? L"已切换到 1 分钟分时走势。" : L"已切换到日 K 线。");
}

void showError(AppState& state, const std::exception& ex) {
    logger().write("ERROR", ex.what());
    MessageBoxW(state.window, utf8ToWide(ex.what()).c_str(), L"执行失败", MB_ICONERROR);
    setStatus(state, L"执行失败，请稍后重试。");
}

void openLogDirectory(AppState& state) {
    std::filesystem::path directory = applicationStorageDirectory() / L"logs";
    std::filesystem::create_directories(directory);
    HINSTANCE result = ShellExecuteW(state.window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        throw std::runtime_error("无法打开日志目录。");
    }
    setStatus(state, L"已打开运行日志目录。");
}

}  // namespace ashare
