#include "dashboard_view.h"

#include "main_window.h"
#include "market_controller.h"
#include "resource_ids.h"
#include "trade_ledger.h"
#include "utils.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>
#include <vector>

namespace ashare {

std::wstring getEditText(HWND edit) {
    int len = GetWindowTextLengthW(edit);
    std::wstring value(len + 1, L'\0');
    GetWindowTextW(edit, value.data(), len + 1);
    value.resize(len);
    return value;
}

std::wstring quoteColumnText(const Quote& quote, int column) {
    // ETF/基金价格显示三位小数，股票和指数显示两位。
    int pricePrecision = isFundLikeSymbol(quote.symbol) ? 3 : 2;
    switch (column) {
    case 0: return utf8ToWide(quote.symbol);
    case 1: return utf8ToWide(quote.name);
    case 2: return formatNumber(quote.price, pricePrecision);
    case 3: return formatNumber(quote.changePercent) + L"%";
    case 4: return formatNumber(quote.change, pricePrecision);
    case 5: return formatNumber(quote.open, pricePrecision);
    case 6: return formatNumber(quote.previousClose, pricePrecision);
    case 7: return formatLarge(quote.volume);
    case 8: return formatLarge(quote.amount);
    case 9: return utf8ToWide(quote.time);
    default: return L"";
    }
}

void updateListRows(HWND list, const std::vector<std::vector<std::wstring>>& rows) {
    // 关闭重绘后逐单元格比较，只更新发生变化的文本，避免 5 秒刷新时列表抖动。
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    bool changed = false;
    int existingRows = ListView_GetItemCount(list);
    wchar_t current[512]{};
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        const auto& columns = rows[static_cast<size_t>(row)];
        if (columns.empty()) {
            continue;
        }
        if (row >= existingRows) {
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(columns.front().c_str());
            ListView_InsertItem(list, &item);
            ++existingRows;
            changed = true;
        } else {
            current[0] = L'\0';
            ListView_GetItemText(list, row, 0, current, static_cast<int>(std::size(current)));
            if (columns.front() != current) {
                ListView_SetItemText(list, row, 0, const_cast<wchar_t*>(columns.front().c_str()));
                changed = true;
            }
        }
        for (int column = 1; column < static_cast<int>(columns.size()); ++column) {
            current[0] = L'\0';
            ListView_GetItemText(list, row, column, current, static_cast<int>(std::size(current)));
            if (columns[static_cast<size_t>(column)] != current) {
                ListView_SetItemText(list, row, column,
                                     const_cast<wchar_t*>(columns[static_cast<size_t>(column)].c_str()));
                changed = true;
            }
        }
    }
    while (existingRows > static_cast<int>(rows.size())) {
        ListView_DeleteItem(list, --existingRows);
        changed = true;
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    // 没有数据变化时不触发无意义重绘，并保留当前滚动条和选中行。
    if (changed) {
        RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_NOERASE | RDW_UPDATENOW);
    }
}

void populateQuotes(HWND list, const std::vector<Quote>& quotes) {
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(quotes.size());
    for (const auto& quote : quotes) {
        std::vector<std::wstring> columns;
        columns.reserve(10);
        for (int column = 0; column < 10; ++column) {
            columns.push_back(quoteColumnText(quote, column));
        }
        rows.push_back(std::move(columns));
    }
    updateListRows(list, rows);
}

void selectSymbolInList(HWND list, const std::string& symbol) {
    // 仅在选中项真正变化时更新状态，避免自动刷新把滚动位置拉回。
    for (int row = 0; row < ListView_GetItemCount(list); ++row) {
        wchar_t buffer[64]{};
        ListView_GetItemText(list, row, 0, buffer, 64);
        if (wideToUtf8(buffer) == symbol) {
            int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            if (selected != row) {
                ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(list, row, FALSE);
            }
            return;
        }
    }
}

std::optional<std::string> selectedSymbolFromList(HWND list) {
    int selected = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (selected < 0) return std::nullopt;
    wchar_t buffer[64]{};
    ListView_GetItemText(list, selected, 0, buffer, 64);
    return wideToUtf8(buffer);
}

struct ListColumnDefinition {
    const wchar_t* name;
    int width;
};

void setListColumns(HWND list, const std::vector<ListColumnDefinition>& columns) {
    // 页面共用同一个 ListView，切换页面时先清空再创建对应列模型。
    while (Header_GetItemCount(ListView_GetHeader(list)) > 0) {
        ListView_DeleteColumn(list, 0);
    }
    for (int index = 0; index < static_cast<int>(columns.size()); ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[static_cast<size_t>(index)].name);
        column.cx = columns[static_cast<size_t>(index)].width;
        column.iSubItem = index;
        ListView_InsertColumn(list, index, &column);
    }
}

void setQuoteColumns(HWND list) {
    setListColumns(list, {
        {L"代码", 82}, {L"名称", 100}, {L"最新", 82}, {L"涨跌幅", 76}, {L"涨跌额", 76},
        {L"今开", 82}, {L"昨收", 82}, {L"成交量", 95}, {L"成交额", 95}, {L"时间", 105},
    });
}

const Quote* cachedQuote(const AppState& state, const std::string& symbol) {
    auto quote = state.quoteCache.find(symbol);
    return quote == state.quoteCache.end() ? nullptr : &quote->second;
}

std::vector<Quote> cachedQuotesInOrder(const AppState& state, const std::vector<std::string>& symbols) {
    // map 的排序不代表用户顺序，按调用方给出的代码列表重新组装结果。
    std::vector<Quote> quotes;
    quotes.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        if (const Quote* quote = cachedQuote(state, symbol)) {
            quotes.push_back(*quote);
        }
    }
    return quotes;
}

void populateHoldings(AppState& state) {
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(state.holdings.size());
    double totalDailyProfit = 0.0;
    double totalProfit = 0.0;
    double totalCost = 0.0;
    double totalMarketValue = 0.0;
    for (const auto& [symbol, holding] : state.holdings) {
        const Quote* quote = cachedQuote(state, symbol);
        double currentPrice = quote ? quote->price : 0.0;
        double marketValue = currentPrice * static_cast<double>(holding.quantity);
        // 回放历史日期时只应用同一历史日期的成交，避免把今天的实盘成交带入历史盈亏。
        std::string valuationDate = currentDateToken();
        if (state.dataMode == MarketDataMode::Replay && state.replay.ready()) {
            valuationDate = state.replay.date();
            valuationDate.erase(std::remove(valuationDate.begin(), valuationDate.end(), '-'),
                                valuationDate.end());
        }
        long long openingQuantity = holding.tradeBaseDate == valuationDate
                                        ? holding.tradeBaseQuantity
                                        : holding.quantity;
        double openingCost = holding.tradeBaseDate == valuationDate
                                 ? holding.tradeBaseCost
                                 : holding.cost;
        auto trades = tradesFor(state.holdingTrades, symbol, valuationDate);
        TradeLedgerSnapshot ledger = calculateTradeLedger(
            openingQuantity, openingCost, trades, currentPrice,
            quote ? quote->previousClose : 0.0);
        long long displayQuantity = ledger.valid ? ledger.quantity : holding.quantity;
        double displayCost = ledger.valid ? ledger.cost : holding.cost;
        marketValue = currentPrice * static_cast<double>(displayQuantity);
        double dailyProfit = ledger.valid ? ledger.dailyProfit : 0.0;
        double realizedProfit = ledger.valid ? ledger.realizedProfit : 0.0;
        double floatingProfit = ledger.valid ? ledger.floatingProfit : 0.0;
        double totalHoldingProfit = ledger.valid ? ledger.totalProfit : 0.0;
        double costValue = displayCost * static_cast<double>(displayQuantity);
        double profitPercent = costValue > 0.0 ? floatingProfit / costValue * 100.0 : 0.0;
        totalDailyProfit += dailyProfit;
        totalProfit += totalHoldingProfit;
        totalCost += costValue;
        totalMarketValue += marketValue;
        int precision = isFundLikeSymbol(symbol) ? 3 : 2;
        rows.push_back({
            utf8ToWide(symbol), quote ? utf8ToWide(quote->name) : L"--",
            currentPrice > 0.0 ? formatNumber(currentPrice, precision) : L"--",
            std::to_wstring(displayQuantity), formatNumber(displayCost, 3),
            currentPrice > 0.0 ? formatLarge(marketValue) : L"--", formatNumber(dailyProfit),
            formatNumber(realizedProfit), formatNumber(floatingProfit),
            formatNumber(totalHoldingProfit), formatNumber(profitPercent) + L"%",
            quote ? utf8ToWide(quote->time) : L"--",
        });
    }
    updateListRows(state.list, rows);
    std::wostringstream summary;
    double totalPercent = totalCost > 0.0 ? totalProfit / totalCost * 100.0 : 0.0;
    summary << L"持仓  总市值 " << formatLarge(totalMarketValue)
            << L"  总资产 " << formatLarge(state.accountFunds.totalAssets)
            << L"  可用资金 " << formatLarge(state.accountFunds.availableCash)
            << L"  当日盈亏 " << formatNumber(totalDailyProfit)
            << L"  总盈亏 " << formatNumber(totalProfit)
            << L"  总收益率 " << formatNumber(totalPercent) << L"%";
    setWindowTextIfChanged(state.favoritesLabel, summary.str());
}

std::wstring signalTypeText(SignalType type) {
    switch (type) {
    case SignalType::Buy: return L"买入窗口";
    case SignalType::Sell: return L"卖出窗口";
    default: return L"观察";
    }
}

namespace {
std::wstring technicalStateText(const std::string& key) {
    static const std::pair<const char*, const wchar_t*> names[] = {
        {"CONTINUED_DECLINE", L"继续下跌"}, {"BOTTOM_WATCH", L"底部观察"},
        {"BOTTOMING", L"止跌整理"}, {"EARLY_STRENGTHENING", L"初步转强"},
        {"TREND_STRENGTHENING", L"趋势转强"}, {"HOLD", L"普通持有"},
        {"NEAR_RESISTANCE", L"接近压力"}, {"HIGH_WEAKENING", L"高位转弱"},
        {"BREAKOUT", L"有效突破"}, {"DOWNTREND", L"下降结构"},
        {"POSSIBLE_BOTTOM", L"可能筑底"}, {"EARLY_REVERSAL", L"低点抬高"},
        {"UPTREND", L"高低点同时抬高"}, {"RANGE", L"区间震荡"}, {"TOPPING", L"顶部转弱"},
        {"UNKNOWN", L"待确认"}, {"WAIT", L"等待"}, {"WATCH_BUY", L"低吸观察"},
        {"EARLY_BUY", L"初步低吸候选"}, {"BUY_CONFIRMATION", L"低吸确认"},
        {"STRONG_BUY_CONFIRMATION", L"较强低吸确认"}, {"WATCH_SELL", L"高抛观察"},
        {"PARTIAL_SELL", L"部分减仓候选"}, {"SELL_CONFIRMATION", L"减仓确认"},
        {"STRONG_SELL_CONFIRMATION", L"较强减仓确认"}, {"BREAKOUT_HOLD", L"突破后持有观察"},
        {"DO_NOT_AVERAGE_DOWN", L"禁止下跌补仓"}, {"INSUFFICIENT_DATA", L"日线数据不足"},
        {"INVALID_DATA", L"行情数据无效"}, {"INVALID_CONFIG", L"技术参数无效"}, {"DISABLED", L"未开启"},
    };
    for (const auto& [code, text] : names) if (key == code) return text;
    return utf8ToWide(key);
}

std::wstring technicalReasonText(std::string reason) {
    static const std::pair<const char*, const char*> labels[] = {
        {"near_support", "进入有效支撑区"}, {"no_new_low", "近期没有创新低"},
        {"higher_low", "波段低点抬高"}, {"above_ma10", "收盘站上短期均线"},
        {"volume_improving", "成交量改善"}, {"macd_improving", "MACD动能改善"},
        {"break_recent_swing_high", "突破最近波段高点"}, {"near_resistance", "进入有效压力区"},
        {"weak_candle", "K线出现转弱形态"}, {"volume_stagnation", "放量滞涨或冲高回落"},
        {"cross_below_ma5", "当日跌破短均线"}, {"red_hist_shrinking", "MACD红柱连续缩短"},
        {"macd_dead_cross", "MACD死叉"},
    };
    for (const auto& [key, text] : labels) {
        auto pos = reason.find(key);
        if (pos != std::string::npos) reason.replace(pos, std::char_traits<char>::length(key), text);
    }
    return utf8ToWide(reason);
}
}  // namespace

void populateTechnicalSwingPanel(AppState& state) {
    if (!state.technicalSwingDetails || state.detailView != DetailView::Strategy) return;
    auto active = std::find_if(state.strategies.begin(), state.strategies.end(),
        [&](const StrategyConfig& c) { return c.id == state.activeStrategyId; });
    bool enabled = active != state.strategies.end() && active->technicalSwing.enableTechnicalSwingStrategy;
    HWND toggle = GetDlgItem(state.window, IDC_TECHNICAL_SWING_TOGGLE);
    setWindowTextIfChanged(toggle, enabled ? L"关闭技术分析" : L"开启技术分析");
    EnableWindow(toggle, active != state.strategies.end() && !state.strategyScanRunning);
    std::wstring text;
    std::string symbol = selectedSymbolFromList(state.list).value_or(state.currentSymbol);
    if (state.strategySymbols.count(symbol) == 0) {
        text = L"请在下方策略观察表中选择一个标的。\r\n\r\n技术分析与当前算法信号分别展示。";
    } else if (!enabled) {
        text = utf8ToWide(symbol) + L"\r\n\r\n技术面分析尚未开启。\r\n点击上方“开启技术分析”查看趋势、支撑压力、双向评分及原因。\r\n\r\n原策略买卖信号保持不变。";
    } else {
        auto found = std::find_if(state.strategySignals.begin(), state.strategySignals.end(),
            [&](const TradingSignal& s) { return s.symbol == symbol; });
        text = utf8ToWide(symbol);
        if (const Quote* quote = cachedQuote(state, symbol)) text += L"  " + utf8ToWide(quote->name);
        text += L"\r\n";
        if (found == state.strategySignals.end() || !found->technicalSwingResult) {
            text += state.strategyScanRunning ? L"正在分析日线，请稍候…" : L"尚无技术结果，请点击“刷新”。";
            if (!state.strategyScanRunning && found != state.strategySignals.end() && !found->message.empty())
                text += L"\r\n" + utf8ToWide(found->message);
        } else {
            const auto& r = *found->technicalSwingResult;
            text += L"日线日期 " + utf8ToWide(r.date) +
                    (state.dataMode == MarketDataMode::Replay ? L" · 回放时点" : L" · 实盘日线快照");
            if (state.strategyScanRunning) text += L" · 更新中";
            text += L"\r\n技术信号：" + technicalStateText(r.technicalSignal);
            bool valid = r.technicalSignal != "INSUFFICIENT_DATA" && r.technicalSignal != "INVALID_DATA" &&
                         r.technicalSignal != "INVALID_CONFIG" && r.technicalSignal != "DISABLED";
            if (valid) {
                int precision = isFundLikeSymbol(symbol) ? 3 : 2;
                auto price = [&](std::optional<double> p) { return p ? formatNumber(*p, precision) : L"未确认"; };
                auto distance = [&](std::optional<double> d) { return d ? formatNumber(*d * 100, 2) + L"%" : L"--"; };
                text += L"\r\n趋势：" + technicalStateText(r.trend) + L"  /  " + technicalStateText(r.priceStructure);
                text += L"\r\n低吸 " + std::to_wstring(r.buyScore) + L"/7  · 强度 " + formatNumber(r.buyStrength, 2) +
                        L"\r\n高抛 " + std::to_wstring(r.sellScore) + L"/6  · 强度 " + formatNumber(r.sellStrength, 2);
                text += L"\r\n支撑 " + price(r.nearestSupport) + L"  · 距离 " + distance(r.distanceToSupportPct);
                text += L"\r\n压力 " + price(r.nearestResistance) + L"  · 距离 " + distance(r.distanceToResistancePct);
                text += L"\r\n次支撑 " + price(r.secondSupport) + L"    次压力 " + price(r.secondResistance);
                if (r.buyBlocked) text += L"\r\n低吸已阻止；原始评分仅供参考。";
                text += L"\r\n低吸状态：" + technicalStateText(r.buySignal) +
                        L"\r\n高抛状态：" + technicalStateText(r.sellSignal);
                if (r.breakout) text += r.confirmedBreakout ? L"\r\n突破：连续站稳确认" : L"\r\n突破：等待持续站稳";
                if (r.falseBreakout) text += L"\r\n风险：压力位假突破";
                if (r.supportRecovered) text += L"\r\n支撑：盘中跌破后收回";
                if (r.supportBreakdown) text += L"\r\n风险：放量有效跌破支撑";
            }
            text += L"\r\n\r\n判断原因\r\n";
            for (const auto& reason : r.reasons) text += L"• " + technicalReasonText(reason) + L"\r\n";
            text += L"\r\n技术因子独立展示，最终信号见观察表。";
        }
    }
    // 相同内容不重置选择和滚动位置；更新时尽量保留用户阅读位置。
    if (getEditText(state.technicalSwingDetails) != text) {
        LRESULT firstLine = getEditText(state.technicalSwingDetails).rfind(utf8ToWide(symbol), 0) == 0
            ? SendMessageW(state.technicalSwingDetails, EM_GETFIRSTVISIBLELINE, 0, 0) : 0;
        SetWindowTextW(state.technicalSwingDetails, text.c_str());
        SendMessageW(state.technicalSwingDetails, EM_LINESCROLL, 0, firstLine);
    }
}

void populateStrategySignals(AppState& state) {
    std::vector<std::vector<std::wstring>> rows;
    auto active = std::find_if(state.strategies.begin(), state.strategies.end(),
                               [&](const StrategyConfig& config) { return config.id == state.activeStrategyId; });
    std::string activeName = active != state.strategies.end() ? active->name : "未选择策略";
    bool activeT0 = active != state.strategies.end() &&
                    active->kind == StrategyKind::T0Intraday;
    for (const auto& symbol : state.strategySymbols) {
        std::vector<TradingSignal> symbolSignals;
        std::copy_if(state.strategySignals.begin(), state.strategySignals.end(),
                     std::back_inserter(symbolSignals),
                     [&](const TradingSignal& signal) { return signal.symbol == symbol; });
        // 后台结果尚未返回时插入占位行，避免列表在扫描期间跳空。
        if (symbolSignals.empty()) {
            TradingSignal placeholder;
            placeholder.symbol = symbol;
            placeholder.algorithm = activeName;
            placeholder.message = state.strategyScanRunning ? "分析中" : "等待扫描";
            if (const Quote* quote = cachedQuote(state, symbol)) {
                placeholder.name = quote->name;
                placeholder.currentPrice = quote->price;
                placeholder.time = quote->time;
            }
            symbolSignals.push_back(std::move(placeholder));
        }
        for (const auto& signal : symbolSignals) {
            int precision = isFundLikeSymbol(symbol) ? 3 : 2;
            std::wstring window = signal.windowLow > 0.0
                                      ? formatNumber(signal.windowLow, precision) + L" - " +
                                            formatNumber(signal.windowHigh, precision)
                                      : L"--";
            std::optional<SimulationSnapshot> simulation;
            std::optional<TradeLedgerSnapshot> liveLedger;
            if (state.dataMode == MarketDataMode::Replay && state.replay.ready()) {
                double currentPrice = signal.currentPrice;
                if (currentPrice <= 0.0) {
                    if (const Quote* quote = cachedQuote(state, symbol)) {
                        currentPrice = quote->price;
                    }
                }
                simulation = state.strategySimulator.snapshot(state.activeStrategyId, symbol,
                                                              currentPrice);
            } else {
                auto holding = state.holdings.find(symbol);
                long long openingQuantity = 0;
                double openingCost = 0.0;
                if (holding != state.holdings.end()) {
                    openingQuantity = holding->second.tradeBaseDate == currentDateToken()
                                          ? holding->second.tradeBaseQuantity
                                          : holding->second.quantity;
                    openingCost = holding->second.tradeBaseDate == currentDateToken()
                                      ? holding->second.tradeBaseCost
                                      : holding->second.cost;
                }
                auto trades = effectiveStrategyTrades(state.strategyTrades,
                                                       state.activeStrategyId, symbol,
                                                       currentDateToken());
                double currentPrice = signal.currentPrice;
                if (currentPrice <= 0.0) {
                    if (const Quote* quote = cachedQuote(state, symbol)) currentPrice = quote->price;
                }
                const Quote* quote = cachedQuote(state, symbol);
                liveLedger = calculateTradeLedger(openingQuantity, openingCost, trades,
                                                  currentPrice,
                                                  quote ? quote->previousClose : 0.0);
            }
            std::optional<double> t0Profit;
            if (activeT0) {
                if (simulation) {
                    t0Profit = simulation->t0Profit;
                } else if (liveLedger) t0Profit = liveLedger->closedT0Profit;
            }
            long long displayQuantity = simulation ? simulation->quantity
                                                    : liveLedger ? liveLedger->quantity : 0;
            double displayCost = simulation ? simulation->cost
                                            : liveLedger ? liveLedger->cost : 0.0;
            double dailyProfit = simulation ? simulation->dailyProfit
                                            : liveLedger ? liveLedger->dailyProfit : 0.0;
            double floatingProfit = liveLedger ? liveLedger->floatingProfit : 0.0;
            double totalProfit = simulation ? simulation->totalProfit
                                            : liveLedger ? liveLedger->totalProfit : 0.0;
            double initialValue = liveLedger
                                      ? liveLedger->openingCost * liveLedger->openingQuantity
                                      : 0.0;
            double returnPercent = simulation ? simulation->totalReturnPercent
                                              : initialValue > 0.0 ? totalProfit / initialValue * 100.0 : 0.0;
            int tradeCount = simulation ? simulation->tradeCount
                                        : liveLedger ? liveLedger->tradeCount : 0;
            rows.push_back({
                utf8ToWide(symbol), utf8ToWide(signal.name),
                signal.currentPrice > 0.0 ? formatNumber(signal.currentPrice, precision) : L"--",
                utf8ToWide(signal.algorithm), signalTypeText(signal.type),
                std::to_wstring(displayQuantity),
                displayQuantity > 0 ? formatNumber(displayCost, 3) : L"--",
                formatNumber(dailyProfit), formatNumber(floatingProfit),
                t0Profit ? formatNumber(*t0Profit) : L"--",
                formatNumber(totalProfit), formatNumber(returnPercent) + L"%",
                std::to_wstring(tradeCount),
                window, utf8ToWide(signal.message), utf8ToWide(signal.time),
            });
        }
    }
    updateListRows(state.list, rows);
    std::wstring title;
    if (state.dataMode == MarketDataMode::Replay && state.replay.ready()) {
        SimulationSummary summary = state.strategySimulator.summary(state.activeStrategyId,
                                                                     state.quoteCache);
        title = L"策略收益分析  " + utf8ToWide(activeName) +
                L"  资产 " + formatLarge(summary.equity) +
                L"  当日盈亏 " + formatNumber(summary.dailyProfit) +
                (activeT0 ? L"  T0净盈亏 " + formatNumber(summary.t0Profit) : L"") +
                L"  总盈亏 " + formatNumber(summary.totalProfit) +
                L"  收益率 " + formatNumber(summary.totalReturnPercent) + L"%" +
                L"  费用 " + formatNumber(summary.totalFees) +
                L"  成交 " + std::to_wstring(summary.tradeCount) + L" 笔";
    } else {
        title = L"策略观察  " + utf8ToWide(activeName) + L"  " +
                std::to_wstring(state.strategySymbols.size()) + L" 个标的";
        double totalDailyProfit = 0.0;
        double totalT0Profit = 0.0;
        double totalFloatingProfit = 0.0;
        double totalProfit = 0.0;
        double totalFees = 0.0;
        int tradeCount = 0;
        for (const auto& symbol : state.strategySymbols) {
            auto holding = state.holdings.find(symbol);
            long long openingQuantity = holding == state.holdings.end() ? 0 :
                holding->second.tradeBaseDate == currentDateToken()
                    ? holding->second.tradeBaseQuantity : holding->second.quantity;
            double openingCost = holding == state.holdings.end() ? 0.0 :
                holding->second.tradeBaseDate == currentDateToken()
                    ? holding->second.tradeBaseCost : holding->second.cost;
            auto trades = effectiveStrategyTrades(state.strategyTrades, state.activeStrategyId,
                                                   symbol, currentDateToken());
            const Quote* quote = cachedQuote(state, symbol);
            auto ledger = calculateTradeLedger(openingQuantity, openingCost, trades,
                                               quote ? quote->price : 0.0,
                                               quote ? quote->previousClose : 0.0);
            if (!ledger.valid) continue;
            totalDailyProfit += ledger.dailyProfit;
            totalT0Profit += ledger.closedT0Profit;
            totalFloatingProfit += ledger.floatingProfit;
            totalProfit += ledger.totalProfit;
            totalFees += ledger.totalFees;
            tradeCount += ledger.tradeCount;
        }
        title += L"  当日盈亏 " + formatNumber(totalDailyProfit) +
                 L"  浮动盈亏 " + formatNumber(totalFloatingProfit);
        if (activeT0) title += L"  T0净盈亏 " + formatNumber(totalT0Profit);
        title += L"  总盈亏 " + formatNumber(totalProfit) +
                 L"  费用 " + formatNumber(totalFees) +
                 L"  成交 " + std::to_wstring(tradeCount) + L" 笔";
    }
    if (state.strategyScanRunning) {
        title += L"  分析中...";
    }
    setWindowTextIfChanged(state.favoritesLabel, title);
    populateTechnicalSwingPanel(state);
}

void configureSignalHistoryColumns(AppState& state) {
    if (state.detailView == DetailView::Holdings ||
        (state.detailView == DetailView::Strategy && state.strategyDetailsShowTrades)) {
        setListColumns(state.signalHistoryList, {
            {L"时间", 120}, {L"代码", 82}, {L"名称", 96}, {L"方向", 58},
            {L"数量", 72}, {L"成交价", 76}, {L"成交额", 92}, {L"佣金", 68},
            {L"印花税", 68}, {L"过户费", 68}, {L"其他费用", 76}, {L"来源", 76},
        });
        return;
    }
    setListColumns(state.signalHistoryList, {
        {L"时间", 118}, {L"来源", 58}, {L"方向", 64}, {L"代码", 82},
        {L"名称", 92}, {L"策略", 112}, {L"触发价", 76}, {L"建议数量", 78},
        {L"参考价格窗口", 122}, {L"预计费用", 78}, {L"预期净收益", 92}, {L"说明", 300},
    });
}

void populateSignalHistory(AppState& state) {
    if (state.detailView != DetailView::Strategy || state.strategyDetailsShowTrades) return;
    std::vector<std::vector<std::wstring>> rows;
    const auto& records = state.strategySignalHistory.records();
    rows.reserve(records.size());
    // 最新信号放在最上方，播放过程中无需滚动即可看到刚触发的记录。
    for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
        const TradingSignal& signal = iterator->signal;
        int precision = isFundLikeSymbol(signal.symbol) ? 3 : 2;
        std::wstring window = signal.windowLow > 0.0
                                  ? formatNumber(signal.windowLow, precision) + L" - " +
                                        formatNumber(signal.windowHigh, precision)
                                  : L"--";
        rows.push_back({
            utf8ToWide(signal.time),
            iterator->dataMode == MarketDataMode::Replay ? L"回放" : L"实盘",
            signal.type == SignalType::Buy ? L"买入" : L"卖出",
            utf8ToWide(signal.symbol), utf8ToWide(signal.name), utf8ToWide(signal.algorithm),
            signal.currentPrice > 0.0 ? formatNumber(signal.currentPrice, precision) : L"--",
            signal.suggestedQuantity > 0 ? std::to_wstring(signal.suggestedQuantity) : L"--",
            window,
            signal.estimatedCost > 0.0 ? formatNumber(signal.estimatedCost) : L"--",
            signal.expectedNetProfit != 0.0 ? formatNumber(signal.expectedNetProfit) : L"--",
            utf8ToWide(signal.message),
        });
    }
    updateListRows(state.signalHistoryList, rows);
    std::wstring title = records.empty()
                             ? L"交易信号明细  等待交易信号"
                             : L"交易信号明细  " + std::to_wstring(records.size()) + L" 条";
    setWindowTextIfChanged(state.signalHistoryLabel, title);
}

void populateTradeDetails(AppState& state) {
    if (state.detailView == DetailView::Favorites ||
        (state.detailView == DetailView::Strategy && !state.strategyDetailsShowTrades)) return;
    std::vector<TradeRecord> records;
    const std::string date = currentDateToken();
    if (state.detailView == DetailView::Holdings) {
        for (const auto& trade : state.holdingTrades) {
            if (tradeDateToken(trade.time) == date) records.push_back(trade);
        }
    } else {
        for (const auto& symbol : state.strategySymbols) {
            auto effective = effectiveStrategyTrades(state.strategyTrades, state.activeStrategyId,
                                                      symbol, date);
            records.insert(records.end(), effective.begin(), effective.end());
        }
    }
    std::stable_sort(records.begin(), records.end(), [](const auto& left, const auto& right) {
        return left.time != right.time ? left.time > right.time : left.id > right.id;
    });
    std::vector<std::vector<std::wstring>> rows;
    rows.reserve(records.size());
    for (const auto& trade : records) {
        const Quote* quote = cachedQuote(state, trade.symbol);
        int precision = isFundLikeSymbol(trade.symbol) ? 3 : 2;
        rows.push_back({
            utf8ToWide(trade.time), utf8ToWide(trade.symbol),
            quote ? utf8ToWide(quote->name) : L"--",
            trade.side == SignalType::Buy ? L"买入" : L"卖出",
            std::to_wstring(trade.quantity), formatNumber(trade.price, precision),
            formatNumber(trade.price * static_cast<double>(trade.quantity)),
            formatNumber(trade.commission), formatNumber(trade.stampDuty),
            formatNumber(trade.transferFee), formatNumber(trade.otherFees),
            trade.source == TradeSource::Manual ? L"手工录入" : L"算法模拟",
        });
    }
    updateListRows(state.signalHistoryList, rows);
    std::wstring title = state.detailView == DetailView::Holdings
                             ? L"当日成交明细"
                             : L"策略成交明细";
    title += records.empty() ? L"  暂无成交" : L"  " + std::to_wstring(records.size()) + L" 笔";
    setWindowTextIfChanged(state.signalHistoryLabel, title);
}

void configureDetailColumns(AppState& state) {
    switch (state.detailView) {
    case DetailView::Favorites:
        setQuoteColumns(state.list);
        break;
    case DetailView::Holdings:
        setListColumns(state.list, {
            {L"代码", 82}, {L"名称", 95}, {L"最新", 70}, {L"数量", 75}, {L"成本", 75},
            {L"市值", 90}, {L"当日盈亏", 90}, {L"已实现盈亏", 92}, {L"浮动盈亏", 90},
            {L"总盈亏", 90}, {L"收益率", 76}, {L"时间", 110},
        });
        break;
    case DetailView::Strategy:
        setListColumns(state.list, {
            {L"代码", 82}, {L"名称", 88}, {L"最新", 68}, {L"算法", 120}, {L"信号", 82},
            {L"策略持仓", 82}, {L"策略成本", 82}, {L"当日盈亏", 88},
            {L"浮动盈亏", 88}, {L"T0净盈亏", 88}, {L"总盈亏", 88},
            {L"收益率", 76}, {L"成交", 58},
            {L"参考价格窗口", 120}, {L"说明", 260}, {L"时间", 105},
        });
        break;
    case DetailView::Screener:
    case DetailView::Research:
    case DetailView::TradeAnalysis:
        break;
    }
}

void populateDetailView(AppState& state) {
    state.updatingLists = true;
    switch (state.detailView) {
    case DetailView::Favorites:
        populateQuotes(state.list, cachedQuotesInOrder(
                                      state, std::vector<std::string>(state.favorites.begin(), state.favorites.end())));
        setWindowTextIfChanged(state.favoritesLabel, L"自选行情");
        break;
    case DetailView::Holdings:
        populateHoldings(state);
        break;
    case DetailView::Strategy:
        populateStrategySignals(state);
        break;
    case DetailView::Screener:
    case DetailView::Research:
    case DetailView::TradeAnalysis:
        break;
    }
    if (state.detailView == DetailView::Holdings ||
        (state.detailView == DetailView::Strategy && state.strategyDetailsShowTrades)) {
        populateTradeDetails(state);
    } else if (state.detailView == DetailView::Strategy) {
        populateSignalHistory(state);
    }
    if (state.detailView == DetailView::Favorites) {
        bool indexSelected = std::find(majorIndexSymbols().begin(), majorIndexSymbols().end(),
                                       state.currentSymbol) != majorIndexSymbols().end();
        if (indexSelected) {
            ListView_SetItemState(state.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            selectSymbolInList(state.indexList, state.currentSymbol);
        } else {
            ListView_SetItemState(state.indexList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            selectSymbolInList(state.list, state.currentSymbol);
        }
    } else {
        selectSymbolInList(state.list, state.currentSymbol);
    }
    state.updatingLists = false;
    updateHoldingSummary(state);
}

void populateStrategyCombo(AppState& state) {
    // ITEMDATA 保存 strategies 中的索引，显示名称可改但选择映射保持稳定。
    SendMessageW(state.strategyCombo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(state.strategyCombo, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (int index = 0; index < static_cast<int>(state.strategies.size()); ++index) {
        std::wstring name = utf8ToWide(state.strategies[static_cast<size_t>(index)].name);
        int item = static_cast<int>(SendMessageW(state.strategyCombo, CB_ADDSTRING, 0,
                                                 reinterpret_cast<LPARAM>(name.c_str())));
        SendMessageW(state.strategyCombo, CB_SETITEMDATA, item, index);
        if (state.strategies[static_cast<size_t>(index)].id == state.activeStrategyId) {
            selected = item;
        }
    }
    SendMessageW(state.strategyCombo, CB_SETCURSEL, selected, 0);
    SendMessageW(state.strategyCombo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.strategyCombo, nullptr, FALSE);
}

void selectActiveStrategyFromCombo(AppState& state) {
    int selected = static_cast<int>(SendMessageW(state.strategyCombo, CB_GETCURSEL, 0, 0));
    if (selected == CB_ERR) {
        return;
    }
    int index = static_cast<int>(SendMessageW(state.strategyCombo, CB_GETITEMDATA, selected, 0));
    if (index < 0 || index >= static_cast<int>(state.strategies.size())) {
        return;
    }
    const std::string& id = state.strategies[static_cast<size_t>(index)].id;
    if (id == state.activeStrategyId) {
        return;
    }
    state.activeStrategyId = id;
    state.strategyConfigStore.saveActive(id);
    state.strategySignals.clear();
    state.chartTradeMarkers.clear();
    state.hoveredTradeMarker.reset();
    populateStrategySignals(state);
    if (state.strategyDetailsShowTrades) {
        populateTradeDetails(state);
    }
    InvalidateRect(state.kline, nullptr, FALSE);
    setStatus(state, L"已切换策略：" + utf8ToWide(state.strategies[static_cast<size_t>(index)].name));
    if (!state.strategyScanRunning) {
        startStrategyScan(state);
    }
}

void updateHoldingSummary(AppState& state) {
    if (!state.holdingSummary) {
        return;
    }
    std::wstring summary;
    const std::string& symbol = state.currentSymbol;
    if (symbol.empty()) {
        summary = state.detailView == DetailView::Strategy
                      ? L"未选择策略标的" : L"尚未建立持仓";
    } else {
        summary = utf8ToWide(symbol);
        auto name = state.symbolNames.find(symbol);
        if (name != state.symbolNames.end() && !name->second.empty()) {
            summary += L"  " + utf8ToWide(name->second);
        }
        auto holding = state.holdings.find(symbol);
        if (holding == state.holdings.end()) {
            summary += L"    未录入持仓基准";
        } else {
            long long quantity = holding->second.tradeBaseDate == currentDateToken()
                                     ? holding->second.tradeBaseQuantity
                                     : holding->second.quantity;
            double cost = holding->second.tradeBaseDate == currentDateToken()
                              ? holding->second.tradeBaseCost
                              : holding->second.cost;
            summary += state.detailView == DetailView::Strategy ? L"    底仓 " : L"    持仓 ";
            summary += std::to_wstring(quantity) + L" 股    成本 " + formatNumber(cost, 3);
        }
    }
    setWindowTextIfChanged(state.holdingSummary, summary);
}

void updateToolbarVisibility(AppState& state) {
    // 三个页面复用顶部工具栏，只显示当前工作流需要的输入项和命令。
    bool favoritesView = state.detailView == DetailView::Favorites;
    bool holdingsView = state.detailView == DetailView::Holdings;
    bool strategyView = state.detailView == DetailView::Strategy;
    bool screenerView = state.detailView == DetailView::Screener;
    bool researchView = state.detailView == DetailView::Research;
    bool tradeAnalysisView = state.detailView == DetailView::TradeAnalysis;
    bool standaloneView = screenerView || researchView || tradeAnalysisView;
    auto show = [&](int id, bool visible) {
        ShowWindow(GetDlgItem(state.window, id), visible ? SW_SHOW : SW_HIDE);
    };
    bool holdingEditorVisible = holdingsView || strategyView;
    ShowWindow(state.edit, favoritesView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.quantityEdit, SW_HIDE);
    ShowWindow(state.costEdit, SW_HIDE);
    ShowWindow(state.codeLabel, favoritesView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.quantityLabel, SW_HIDE);
    ShowWindow(state.costLabel, SW_HIDE);
    ShowWindow(state.strategyLabel, strategyView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.strategyCombo, strategyView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.holdingSummary, holdingEditorVisible ? SW_SHOW : SW_HIDE);
    ShowWindow(state.dataModeLabel, standaloneView ? SW_HIDE : SW_SHOW);
    ShowWindow(state.dataModeCombo, standaloneView ? SW_HIDE : SW_SHOW);
    bool replayMode = state.dataMode == MarketDataMode::Replay;
    ShowWindow(state.replayDateLabel, replayMode && !standaloneView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.replayDateEdit, replayMode && !standaloneView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.indexLabel, favoritesView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.indexList, favoritesView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.signalHistoryLabel, holdingsView || strategyView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.signalHistoryList, holdingsView || strategyView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.technicalSwingLabel, strategyView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.technicalSwingDetails, strategyView ? SW_SHOW : SW_HIDE);
    show(IDC_TECHNICAL_SWING_TOGGLE, strategyView);
    show(IDC_SEARCH, favoritesView);
    show(IDC_ADD, favoritesView);
    show(IDC_TOGGLE_STRATEGY, favoritesView);
    show(IDC_SAVE_HOLDING, holdingEditorVisible);
    show(IDC_TODAY_TRADES, holdingsView || strategyView);
    show(IDC_ACCOUNT_FUNDS, holdingsView);
    show(IDC_DETAIL_TOGGLE, strategyView);
    show(IDC_MANAGE_STRATEGIES, strategyView);
    show(IDC_DELETE, !standaloneView);
    show(IDC_REFRESH, !standaloneView);
    show(IDC_OPEN_LOGS, true);
    show(IDC_VIEW_FAVORITES, true);
    show(IDC_VIEW_HOLDINGS, true);
    show(IDC_VIEW_STRATEGY, true);
    show(IDC_VIEW_SCREENER, true);
    show(IDC_VIEW_RESEARCH, true);
    show(IDC_VIEW_TRADE_ANALYSIS, true);
    show(IDC_REPLAY_LOAD, replayMode && !standaloneView);
    // 行情总览和持仓页只负责加载并显示指定日期的历史行情。
    // 播放、单步、重置会推进策略模拟，只在策略观察页开放。
    bool replayTransportVisible = replayMode && strategyView;
    show(IDC_REPLAY_PLAY, replayTransportVisible);
    show(IDC_REPLAY_STEP, replayTransportVisible);
    show(IDC_REPLAY_RESET, replayTransportVisible);
    show(IDC_DAY_KLINE, !standaloneView);
    show(IDC_MIN_KLINE, !standaloneView);
    ShowWindow(state.kline, standaloneView ? SW_HIDE : SW_SHOW);
    ShowWindow(state.favoritesLabel, standaloneView ? SW_HIDE : SW_SHOW);
    ShowWindow(state.list, standaloneView ? SW_HIDE : SW_SHOW);
    ShowWindow(state.status, standaloneView ? SW_HIDE : SW_SHOW);
    ShowWindow(state.screenerView, screenerView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.researchView, researchView ? SW_SHOW : SW_HIDE);
    ShowWindow(state.tradeAnalysisView, tradeAnalysisView ? SW_SHOW : SW_HIDE);
    SetWindowTextW(GetDlgItem(state.window, IDC_DELETE),
                    holdingsView ? L"删除持仓" : state.detailView == DetailView::Strategy ? L"移出观察" : L"删除");
    SetWindowTextW(GetDlgItem(state.window, IDC_TODAY_TRADES), L"成交管理");
    SetWindowTextW(GetDlgItem(state.window, IDC_SAVE_HOLDING),
                    strategyView ? L"编辑底仓" : L"编辑持仓");
    SetWindowTextW(GetDlgItem(state.window, IDC_DETAIL_TOGGLE),
                    state.strategyDetailsShowTrades ? L"信号明细" : L"成交明细");
    updateHoldingSummary(state);
}

}  // namespace ashare
