#include "trade_analysis_view.h"

#include "broker_statement.h"
#include "chart_view.h"
#include "position_advisor.h"
#include "quote_provider.h"
#include "resource_ids.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ashare {
namespace {

constexpr UINT_PTR analysisRefreshTimer = 6100;
constexpr auto dailyCacheLifetime = std::chrono::minutes(10);

struct AdvicePendingResult {
    std::vector<PositionAdvice> advice;
    std::map<std::string, std::vector<KLine>> dailyCache;
    std::vector<std::string> issues;
    std::string error;
    bool dailyRefreshed = false;
};

struct AdviceMailbox {
    std::mutex mutex;
    std::optional<AdvicePendingResult> pending;
    std::atomic_bool alive{true};
    std::atomic<HWND> window{nullptr};
};

struct TradeAnalysisViewState {
    AppState* app = nullptr;
    HWND window = nullptr;
    HWND importButton = nullptr;
    HWND syncButton = nullptr;
    HWND refreshButton = nullptr;
    HWND issuesButton = nullptr;
    HWND summaryButton = nullptr;
    HWND detailButton = nullptr;
    HWND status = nullptr;
    HWND adviceList = nullptr;
    HWND tradeList = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HFONT metricFont = nullptr;
    HFONT smallFont = nullptr;
    BrokerStatement statement;
    BrokerStatementAnalysis analysis;
    std::vector<PositionAdvice> advice;
    std::vector<std::string> issues;
    std::map<std::string, std::vector<KLine>> dailyCache;
    std::chrono::steady_clock::time_point dailyUpdated{};
    std::shared_ptr<AdviceMailbox> mailbox = std::make_shared<AdviceMailbox>();
    bool running = false;
    bool showTradeDetails = false;
    bool draggingSplitter = false;
    double adviceRatio = 0.52;
};

TradeAnalysisViewState* viewState(HWND window) {
    return reinterpret_cast<TradeAnalysisViewState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

void setText(HWND control, const std::wstring& text) {
    int length = GetWindowTextLengthW(control);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, current.data(), length + 1);
    current.resize(static_cast<size_t>(length));
    if (current != text) SetWindowTextW(control, text.c_str());
}

std::wstring number(double value, int precision = 2) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::wstring signedNumber(double value, int precision = 2) {
    return (value > 0.0 ? L"+" : L"") + number(value, precision);
}

std::wstring priceRange(double low, double high, int precision) {
    if (low <= 0.0 || high <= 0.0) return L"--";
    return number(low, precision) + L" - " + number(high, precision);
}

std::wstring dateText(const std::string& value) {
    if (value.size() != 8) return utf8ToWide(value);
    return utf8ToWide(value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2));
}

void addColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.fmt = LVCFMT_LEFT;
    ListView_InsertColumn(list, index, &column);
}

void clearColumns(HWND list) {
    while (Header_GetItemCount(ListView_GetHeader(list)) > 0) {
        ListView_DeleteColumn(list, 0);
    }
}

void appendRow(HWND list, const std::vector<std::wstring>& columns) {
    if (columns.empty()) return;
    int row = ListView_GetItemCount(list);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<LPWSTR>(columns.front().c_str());
    ListView_InsertItem(list, &item);
    for (int column = 1; column < static_cast<int>(columns.size()); ++column) {
        ListView_SetItemText(list, row, column,
                             const_cast<LPWSTR>(columns[static_cast<size_t>(column)].c_str()));
    }
}

void configureAdviceColumns(HWND list) {
    clearColumns(list);
    const struct { const wchar_t* title; int width; } columns[] = {
        {L"代码", 84}, {L"名称", 132}, {L"最新", 76}, {L"成本", 76},
        {L"浮动盈亏", 96}, {L"收益率", 78}, {L"MA5", 72}, {L"MA10", 72},
        {L"MA20", 72}, {L"量比", 64}, {L"MACD柱", 78}, {L"当前建议", 88},
        {L"补仓价格区间", 128}, {L"补仓数量", 88}, {L"减仓价格区间", 128},
        {L"减仓数量", 88}, {L"置信度", 68}, {L"技术依据与仓位约束", 390},
        {L"行情时间", 132},
    };
    for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
        addColumn(list, index, columns[index].title, columns[index].width);
    }
}

void configureSummaryColumns(HWND list) {
    clearColumns(list);
    const struct { const wchar_t* title; int width; } columns[] = {
        {L"代码", 86}, {L"名称", 150}, {L"成交笔数", 76}, {L"买入数量", 92},
        {L"卖出数量", 92}, {L"区间净数量", 96}, {L"买入均价", 86},
        {L"卖出均价", 86}, {L"费用", 78}, {L"区间匹配数量", 106},
        {L"区间匹配盈亏", 108}, {L"匹配收益率", 94}, {L"未匹配卖出", 96},
        {L"净现金流", 104},
    };
    for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
        addColumn(list, index, columns[index].title, columns[index].width);
    }
}

void configureDetailColumns(HWND list) {
    clearColumns(list);
    const struct { const wchar_t* title; int width; } columns[] = {
        {L"日期", 96}, {L"方向", 66}, {L"代码", 84}, {L"名称", 156},
        {L"数量", 82}, {L"成交价", 82}, {L"发生金额", 100}, {L"手续费", 76},
        {L"印花税", 70}, {L"过户费", 70}, {L"资金余额", 104},
    };
    for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
        addColumn(list, index, columns[index].title, columns[index].width);
    }
}

void populateAdvice(TradeAnalysisViewState& state) {
    SendMessageW(state.adviceList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state.adviceList);
    for (const auto& row : state.advice) {
        int precision = isFundLikeSymbol(row.symbol) ? 3 : 2;
        appendRow(state.adviceList, {
            utf8ToWide(row.symbol), utf8ToWide(row.name), number(row.currentPrice, precision),
            number(row.cost, 3), signedNumber(row.floatingProfit),
            signedNumber(row.floatingReturnPercent) + L"%", number(row.ma5, precision),
            number(row.ma10, precision), number(row.ma20, precision), number(row.volumeRatio),
            signedNumber(row.macdHistogram, 4), utf8ToWide(row.actionText),
            priceRange(row.addPriceLow, row.addPriceHigh, precision),
            row.addQuantity > 0 ? std::to_wstring(row.addQuantity) : L"--",
            priceRange(row.reducePriceLow, row.reducePriceHigh, precision),
            row.reduceQuantity > 0 ? std::to_wstring(row.reduceQuantity) : L"--",
            std::to_wstring(row.confidence), utf8ToWide(row.reason), utf8ToWide(row.quoteTime),
        });
    }
    SendMessageW(state.adviceList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.adviceList, nullptr, FALSE);
}

void populateTrades(TradeAnalysisViewState& state) {
    SendMessageW(state.tradeList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state.tradeList);
    if (state.showTradeDetails) {
        configureDetailColumns(state.tradeList);
        for (const auto& trade : state.statement.trades) {
            int precision = isFundLikeSymbol(trade.symbol) ? 3 : 2;
            appendRow(state.tradeList, {
                dateText(trade.date), trade.side == SignalType::Buy ? L"买入" : L"卖出",
                utf8ToWide(trade.symbol), utf8ToWide(trade.name), std::to_wstring(trade.quantity),
                number(trade.price, precision), signedNumber(trade.amount), number(trade.commission),
                number(trade.stampDuty), number(trade.transferFee), number(trade.balance),
            });
        }
    } else {
        configureSummaryColumns(state.tradeList);
        for (const auto& row : state.analysis.symbols) {
            int precision = isFundLikeSymbol(row.symbol) ? 3 : 2;
            appendRow(state.tradeList, {
                utf8ToWide(row.symbol), utf8ToWide(row.name), std::to_wstring(row.tradeCount),
                std::to_wstring(row.buyQuantity), std::to_wstring(row.sellQuantity),
                std::to_wstring(row.netQuantity), number(row.averageBuyPrice, precision),
                number(row.averageSellPrice, precision), number(row.totalFees),
                std::to_wstring(row.matchedQuantity), signedNumber(row.matchedProfit),
                signedNumber(row.matchedReturnPercent) + L"%",
                std::to_wstring(row.unmatchedSellQuantity), signedNumber(row.netCashFlow),
            });
        }
    }
    SendMessageW(state.tradeList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.tradeList, nullptr, FALSE);
    InvalidateRect(state.summaryButton, nullptr, TRUE);
    InvalidateRect(state.detailButton, nullptr, TRUE);
}

bool isHongKongTech(const std::string& symbol, const std::string& name) {
    return symbol == "sh513050" || symbol == "sh513260" ||
           name.find("恒生科技") != std::string::npos ||
           name.find("中概互联网") != std::string::npos;
}

void startAdviceRefresh(TradeAnalysisViewState& state, bool forceDaily) {
    if (state.running || state.app->holdings.empty()) return;
    std::vector<Holding> holdings;
    for (const auto& [symbol, holding] : state.app->holdings) {
        (void)symbol;
        if (holding.quantity > 0) holdings.push_back(holding);
    }
    if (holdings.empty()) return;
    double totalAssets = state.statement.totalAssets > 0.0
                             ? state.statement.totalAssets : state.app->accountFunds.totalAssets;
    double availableCash = state.statement.availableCash > 0.0
                               ? state.statement.availableCash : state.app->accountFunds.availableCash;
    auto now = std::chrono::steady_clock::now();
    bool refreshDaily = forceDaily || state.dailyCache.empty() ||
                        now - state.dailyUpdated >= dailyCacheLifetime;
    state.running = true;
    EnableWindow(state.refreshButton, FALSE);
    setText(state.status, refreshDaily ? L"正在更新实时行情和日线指标..."
                                      : L"正在更新实时行情建议...");
    auto mailbox = state.mailbox;
    auto dailyCache = state.dailyCache;
    std::thread([mailbox, holdings = std::move(holdings), dailyCache = std::move(dailyCache),
                 totalAssets, availableCash, refreshDaily]() mutable {
        AdvicePendingResult pending;
        pending.dailyCache = std::move(dailyCache);
        pending.dailyRefreshed = refreshDaily;
        try {
            QuoteProvider provider;
            std::vector<std::string> symbols;
            for (const auto& holding : holdings) symbols.push_back(holding.symbol);
            auto quotes = provider.fetchQuotes(symbols);
            std::map<std::string, Quote> quoteBySymbol;
            for (auto& quote : quotes) quoteBySymbol[quote.symbol] = std::move(quote);
            if (refreshDaily) {
                for (const auto& holding : holdings) {
                    if (!mailbox->alive.load()) return;
                    try {
                        pending.dailyCache[holding.symbol] =
                            provider.fetchKLines(holding.symbol, 101, 80);
                    } catch (const std::exception& error) {
                        pending.issues.push_back(holding.symbol + " 日线：" + error.what());
                    }
                }
            }

            double correlatedValue = 0.0;
            for (const auto& holding : holdings) {
                auto quote = quoteBySymbol.find(holding.symbol);
                if (quote != quoteBySymbol.end() &&
                    isHongKongTech(holding.symbol, quote->second.name)) {
                    correlatedValue += quote->second.price * holding.quantity;
                }
            }
            double correlatedPercent = totalAssets > 0.0
                                           ? correlatedValue / totalAssets * 100.0 : 0.0;
            for (const auto& holding : holdings) {
                auto quote = quoteBySymbol.find(holding.symbol);
                if (quote == quoteBySymbol.end()) {
                    pending.issues.push_back(holding.symbol + "：实时行情未返回");
                    continue;
                }
                auto daily = pending.dailyCache.find(holding.symbol);
                const std::vector<KLine> empty;
                const auto& lines = daily == pending.dailyCache.end() ? empty : daily->second;
                double exposure = isHongKongTech(holding.symbol, quote->second.name)
                                      ? correlatedPercent : 0.0;
                pending.advice.push_back(evaluatePositionAdvice(
                    holding, quote->second, lines, totalAssets, availableCash, exposure));
            }
            if (pending.advice.empty()) pending.error = "未获得可用的持仓行情。";
        } catch (const std::exception& error) {
            pending.error = error.what();
        }
        if (!mailbox->alive.load()) return;
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->pending = std::move(pending);
        }
        if (HWND window = mailbox->window.load()) {
            PostMessageW(window, WM_APP_TRADE_ANALYSIS_RESULT, 0, 0);
        }
    }).detach();
}

void loadSavedStatement(TradeAnalysisViewState& state) {
    std::filesystem::path path = applicationStorageDirectory() / "broker_statement.csv";
    if (!std::filesystem::exists(path)) return;
    try {
        state.statement = loadBrokerStatementCsv(path);
        state.analysis = analyzeBrokerStatement(state.statement);
        state.issues = state.statement.issues;
        for (const auto& position : state.statement.positions) {
            state.app->symbolNames[position.symbol] = position.name;
        }
        populateTrades(state);
        logger().write("INFO", "Broker statement loaded trades=" +
                                   std::to_string(state.statement.trades.size()) +
                                   " positions=" + std::to_string(state.statement.positions.size()) +
                                   " issues=" + std::to_string(state.statement.issues.size()));
    } catch (const std::exception& error) {
        state.issues.push_back(std::string("历史成交文件：") + error.what());
        logger().write("WARN", std::string("Broker statement load failed: ") + error.what());
    }
}

void importStatement(TradeAnalysisViewState& state) {
    std::vector<wchar_t> file(32768, L'\0');
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = L"券商成交 CSV (*.csv)\0*.csv\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    std::filesystem::path destination = applicationStorageDirectory() / "broker_statement.csv";
    state.statement = importBrokerStatementCsv(file.data(), destination);
    state.analysis = analyzeBrokerStatement(state.statement);
    state.issues = state.statement.issues;
    for (const auto& position : state.statement.positions) {
        state.app->symbolNames[position.symbol] = position.name;
    }
    populateTrades(state);
    std::wstring message = L"已导入 " + std::to_wstring(state.statement.trades.size()) +
                           L" 笔成交、" + std::to_wstring(state.statement.positions.size()) +
                           L" 条持仓。";
    setText(state.status, message);
    logger().write("INFO", "Broker statement imported trades=" +
                               std::to_string(state.statement.trades.size()) +
                               " positions=" + std::to_string(state.statement.positions.size()));
    startAdviceRefresh(state, true);
    InvalidateRect(state.window, nullptr, FALSE);
}

void syncPositions(TradeAnalysisViewState& state) {
    if (state.statement.positions.empty()) {
        setText(state.status, L"当前报表没有可同步的持仓快照。");
        return;
    }
    if (MessageBoxW(state.window,
                    L"将使用券商报表快照替换程序中的持仓数量、成本和资金账户。是否继续？",
                    L"同步持仓与资金", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }
    std::map<std::string, Holding> holdings;
    for (const auto& position : state.statement.positions) {
        Holding holding;
        holding.symbol = position.symbol;
        holding.quantity = position.quantity;
        holding.cost = position.cost;
        holding.tradeBaseQuantity = position.quantity;
        holding.tradeBaseCost = position.cost;
        holdings[position.symbol] = holding;
        state.app->symbolNames[position.symbol] = position.name;
    }
    state.app->holdings = std::move(holdings);
    state.app->holdingTrades.clear();
    state.app->accountFunds.totalAssets = state.statement.totalAssets;
    state.app->accountFunds.availableCash = state.statement.availableCash;
    state.app->accountFunds.updatedAt = state.statement.lastTradeDate;
    state.app->holdingStore.save(state.app->holdings);
    state.app->holdingTradeStore.save(state.app->holdingTrades);
    state.app->accountFundsStore.save(state.app->accountFunds);
    logger().write("INFO", "Broker positions synchronized count=" +
                               std::to_string(state.app->holdings.size()));
    setText(state.status, L"持仓、成本、总资产和可用资金已按券商快照同步。");
    startAdviceRefresh(state, true);
}

void showIssues(const TradeAnalysisViewState& state) {
    std::wstring text;
    if (state.issues.empty()) {
        text = L"当前没有导入或行情错误。";
    } else {
        size_t count = std::min<size_t>(state.issues.size(), 40);
        for (size_t index = 0; index < count; ++index) {
            text += utf8ToWide(state.issues[index]) + L"\r\n";
        }
        if (state.issues.size() > count) text += L"更多信息请查看运行日志。";
    }
    MessageBoxW(state.window, text.c_str(), L"交易分析错误信息",
                MB_OK | MB_ICONINFORMATION);
}

int splitterY(const TradeAnalysisViewState& state, int height) {
    constexpr int contentTop = 144;
    constexpr int lowerMinimum = 170;
    int available = std::max(240, height - contentTop - 12);
    int value = contentTop + static_cast<int>(available * state.adviceRatio);
    return std::clamp(value, contentTop + 140, height - lowerMinimum);
}

void layoutControls(TradeAnalysisViewState& state, int width, int height) {
    constexpr int margin = 10;
    MoveWindow(state.importButton, 24, 24, 112, 36, TRUE);
    MoveWindow(state.syncButton, 146, 24, 124, 36, TRUE);
    MoveWindow(state.refreshButton, 280, 24, 104, 36, TRUE);
    MoveWindow(state.issuesButton, 394, 24, 112, 36, TRUE);
    MoveWindow(state.status, 526, 30, std::max(160, width - 550), 24, TRUE);
    int split = splitterY(state, height);
    MoveWindow(state.adviceList, margin, 144, std::max(100, width - margin * 2),
               std::max(120, split - 150), TRUE);
    MoveWindow(state.summaryButton, 24, split + 16, 104, 34, TRUE);
    MoveWindow(state.detailButton, 138, split + 16, 104, 34, TRUE);
    MoveWindow(state.tradeList, margin, split + 58, std::max(100, width - margin * 2),
               std::max(100, height - split - 68), TRUE);
}

void drawRoundedPanel(HDC dc, const RECT& rect, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void paintView(HWND window, TradeAnalysisViewState& state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    ThemePalette palette = themePalette();
    FillRect(dc, &client, state.windowBrush);
    drawRoundedPanel(dc, RECT{10, 10, client.right - 10, 78}, palette.surface, palette.border);
    drawRoundedPanel(dc, RECT{10, 88, client.right - 10, 134}, palette.surface, palette.border);
    int split = splitterY(state, client.bottom);
    drawRoundedPanel(dc, RECT{10, split + 8, client.right - 10, split + 56},
                     palette.surface, palette.border);
    HPEN splitterPen = CreatePen(PS_SOLID, state.draggingSplitter ? 2 : 1,
                                 state.draggingSplitter ? palette.accent : palette.border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, splitterPen));
    MoveToEx(dc, client.right / 2 - 24, split + 3, nullptr);
    LineTo(dc, client.right / 2 + 24, split + 3);
    SelectObject(dc, oldPen);
    DeleteObject(splitterPen);

    SetBkMode(dc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, state.metricFont));
    SetTextColor(dc, palette.text);
    std::wstring summary;
    if (state.statement.trades.empty()) {
        summary = L"尚未导入券商成交 CSV；导入后显示成交统计、持仓快照和账户数据。";
    } else {
        summary = L"成交 " + std::to_wstring(state.analysis.tradeCount) +
                  L" 笔  买入 " + number(state.analysis.buyNotional) +
                  L"  卖出 " + number(state.analysis.sellNotional) +
                  L"  费用 " + number(state.analysis.totalFees) +
                  L"  区间匹配盈亏 " + signedNumber(state.analysis.matchedProfit) +
                  L"  总资产 " + number(state.statement.totalAssets) +
                  L"  可用资金 " + number(state.statement.availableCash);
    }
    RECT summaryRect{24, 88, client.right - 24, 134};
    DrawTextW(dc, summary.c_str(), -1, &summaryRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(dc, palette.muted);
    SelectObject(dc, state.smallFont);
    RECT labelRect{24, 116, client.right - 24, 142};
    std::wstring label = L"持仓优化：行情5秒更新，日线指标缓存10分钟；价格和数量为条件触发计划，不代表保证成交或收益。";
    DrawTextW(dc, label.c_str(), -1, &labelRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    EndPaint(window, &paint);
}

LRESULT drawList(const TradeAnalysisViewState& state, NMLVCUSTOMDRAW* custom) {
    ThemePalette palette = themePalette();
    switch (custom->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        size_t row = static_cast<size_t>(custom->nmcd.dwItemSpec);
        bool selected = (custom->nmcd.uItemState & CDIS_SELECTED) != 0;
        custom->clrTextBk = selected ? palette.selection
                                     : row % 2 == 0 ? palette.surfaceAlt : palette.surface;
        custom->clrText = palette.text;
        if (custom->nmcd.hdr.hwndFrom == state.adviceList && row < state.advice.size()) {
            const auto& advice = state.advice[row];
            if (custom->iSubItem == 4 || custom->iSubItem == 5) {
                custom->clrText = advice.floatingProfit >= 0.0 ? palette.up : palette.down;
            } else if (custom->iSubItem == 11) {
                custom->clrText = advice.action == PositionAction::Add ? palette.up
                                  : advice.action == PositionAction::Reduce ? palette.down
                                                                           : palette.accent;
            }
        } else if (custom->nmcd.hdr.hwndFrom == state.tradeList) {
            if (!state.showTradeDetails && row < state.analysis.symbols.size() &&
                (custom->iSubItem == 10 || custom->iSubItem == 11)) {
                custom->clrText = state.analysis.symbols[row].matchedProfit >= 0.0
                                      ? palette.up : palette.down;
            } else if (state.showTradeDetails && row < state.statement.trades.size() &&
                       custom->iSubItem == 1) {
                custom->clrText = state.statement.trades[row].side == SignalType::Buy
                                      ? palette.up : palette.down;
            }
        }
        return CDRF_NEWFONT;
    }
    default: return CDRF_DODEFAULT;
    }
}

}  // namespace

LRESULT CALLBACK TradeAnalysisViewProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* app = reinterpret_cast<AppState*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        auto* state = new TradeAnalysisViewState();
        state->app = app;
        state->window = window;
        state->mailbox->window.store(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        ThemePalette palette = themePalette();
        state->windowBrush = CreateSolidBrush(palette.window);
        state->surfaceBrush = CreateSolidBrush(palette.surface);
        state->metricFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                        L"Microsoft YaHei UI");
        state->smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                       L"Microsoft YaHei UI");
        auto button = [&](const wchar_t* text, int id) {
            HWND control = CreateWindowW(L"BUTTON", text,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 0, 0, window, reinterpret_cast<HMENU>(id),
                                         app->instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            return control;
        };
        state->importButton = button(L"导入成交CSV", IDC_ANALYSIS_IMPORT);
        state->syncButton = button(L"同步持仓资金", IDC_ANALYSIS_SYNC);
        state->refreshButton = button(L"刷新建议", IDC_ANALYSIS_REFRESH);
        state->issuesButton = button(L"错误信息 (0)", IDC_ANALYSIS_ISSUES);
        state->summaryButton = button(L"成交汇总", IDC_ANALYSIS_SUMMARY);
        state->detailButton = button(L"成交明细", IDC_ANALYSIS_DETAILS);
        state->status = CreateWindowW(L"STATIC", L"等待导入成交记录。",
                                      WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                      0, 0, 0, 0, window,
                                      reinterpret_cast<HMENU>(IDC_ANALYSIS_STATUS),
                                      app->instance, nullptr);
        SendMessageW(state->status, WM_SETFONT, reinterpret_cast<WPARAM>(state->smallFont), TRUE);
        auto list = [&](int id) {
            HWND control = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                            0, 0, 0, 0, window, reinterpret_cast<HMENU>(id),
                                            app->instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            ListView_SetExtendedListViewStyle(control,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(control, palette.surface);
            ListView_SetTextBkColor(control, palette.surface);
            ListView_SetTextColor(control, palette.text);
            setExplorerControlTheme(control);
            setExplorerControlTheme(ListView_GetHeader(control));
            return control;
        };
        state->adviceList = list(IDC_ANALYSIS_ADVICE_LIST);
        state->tradeList = list(IDC_ANALYSIS_TRADE_LIST);
        SetWindowSubclass(ListView_GetHeader(state->adviceList), HeaderProc, 7,
                          reinterpret_cast<DWORD_PTR>(app));
        SetWindowSubclass(ListView_GetHeader(state->tradeList), HeaderProc, 8,
                          reinterpret_cast<DWORD_PTR>(app));
        configureAdviceColumns(state->adviceList);
        configureSummaryColumns(state->tradeList);
        loadSavedStatement(*state);
        SetTimer(window, analysisRefreshTimer, 5000, nullptr);
        RECT client{};
        GetClientRect(window, &client);
        layoutControls(*state, client.right, client.bottom);
        return 0;
    }

    TradeAnalysisViewState* state = viewState(window);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SHOWWINDOW:
        if (state && wParam) startAdviceRefresh(*state, state->dailyCache.empty());
        return 0;
    case WM_TIMER:
        if (state && wParam == analysisRefreshTimer && IsWindowVisible(window)) {
            startAdviceRefresh(*state, false);
        }
        return 0;
    case WM_SIZE:
        if (state) {
            layoutControls(*state, LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT:
        if (state) {
            paintView(window, *state);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (state) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, themePalette().muted);
            return reinterpret_cast<LRESULT>(state->surfaceBrush);
        }
        break;
    case WM_DRAWITEM:
        if (state) {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                bool primary = item->CtlID == IDC_ANALYSIS_IMPORT ||
                               item->CtlID == IDC_ANALYSIS_REFRESH ||
                               (item->CtlID == IDC_ANALYSIS_SUMMARY && !state->showTradeDetails) ||
                               (item->CtlID == IDC_ANALYSIS_DETAILS && state->showTradeDetails);
                drawModalButton(*item, primary);
                return TRUE;
            }
        }
        break;
    case WM_COMMAND:
        if (!state || HIWORD(wParam) != BN_CLICKED) break;
        try {
            switch (LOWORD(wParam)) {
            case IDC_ANALYSIS_IMPORT: importStatement(*state); break;
            case IDC_ANALYSIS_SYNC: syncPositions(*state); break;
            case IDC_ANALYSIS_REFRESH: startAdviceRefresh(*state, true); break;
            case IDC_ANALYSIS_ISSUES: showIssues(*state); break;
            case IDC_ANALYSIS_SUMMARY:
                state->showTradeDetails = false;
                populateTrades(*state);
                break;
            case IDC_ANALYSIS_DETAILS:
                state->showTradeDetails = true;
                populateTrades(*state);
                break;
            default: break;
            }
        } catch (const std::exception& error) {
            state->issues.push_back(std::string("页面操作：") + error.what());
            setText(state->status, L"操作失败：" + utf8ToWide(error.what()));
            logger().write("ERROR", std::string("Trade analysis action failed: ") + error.what());
        }
        SetWindowTextW(state->issuesButton,
                       (L"错误信息 (" + std::to_wstring(state->issues.size()) + L")").c_str());
        return 0;
    case WM_NOTIFY:
        if (state) {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if ((header->hwndFrom == state->adviceList || header->hwndFrom == state->tradeList) &&
                header->code == NM_CUSTOMDRAW) {
                return drawList(*state, reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            }
        }
        break;
    case WM_SETCURSOR:
        if (state && LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            RECT client{};
            GetClientRect(window, &client);
            if (state->draggingSplitter || std::abs(point.y - splitterY(*state, client.bottom)) <= 6) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (state) {
            RECT client{};
            GetClientRect(window, &client);
            int y = static_cast<short>(HIWORD(lParam));
            if (std::abs(y - splitterY(*state, client.bottom)) <= 8) {
                state->draggingSplitter = true;
                SetCapture(window);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (state && state->draggingSplitter && GetCapture() == window) {
            RECT client{};
            GetClientRect(window, &client);
            int y = static_cast<short>(HIWORD(lParam));
            int available = std::max(240, static_cast<int>(client.bottom) - 156);
            state->adviceRatio = std::clamp(static_cast<double>(y - 144) / available, 0.28, 0.74);
            layoutControls(*state, client.right, client.bottom);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state && state->draggingSplitter) {
            state->draggingSplitter = false;
            if (GetCapture() == window) ReleaseCapture();
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_APP_TRADE_ANALYSIS_RESULT:
        if (state) {
            AdvicePendingResult pending;
            {
                std::lock_guard<std::mutex> lock(state->mailbox->mutex);
                if (!state->mailbox->pending) return 0;
                pending = std::move(*state->mailbox->pending);
                state->mailbox->pending.reset();
            }
            state->running = false;
            EnableWindow(state->refreshButton, TRUE);
            if (pending.dailyRefreshed && !pending.dailyCache.empty()) {
                state->dailyCache = std::move(pending.dailyCache);
                state->dailyUpdated = std::chrono::steady_clock::now();
            }
            state->advice = std::move(pending.advice);
            for (const auto& row : state->advice) {
                state->app->symbolNames[row.symbol] = row.name;
                logger().write("INFO", "Position advice symbol=" + row.symbol +
                                           " action=" + row.actionText +
                                           " add=" + std::to_string(row.addPriceLow) + "-" +
                                           std::to_string(row.addPriceHigh) +
                                           " addQty=" + std::to_string(row.addQuantity) +
                                           " reduce=" + std::to_string(row.reducePriceLow) + "-" +
                                           std::to_string(row.reducePriceHigh) +
                                           " reduceQty=" + std::to_string(row.reduceQuantity));
            }
            for (const auto& issue : pending.issues) state->issues.push_back(issue);
            if (!pending.error.empty()) {
                state->issues.push_back("行情刷新：" + pending.error);
                setText(state->status, L"建议刷新失败：" + utf8ToWide(pending.error));
            } else {
                setText(state->status, L"持仓建议已按最新行情更新，共 " +
                                           std::to_wstring(state->advice.size()) + L" 个标的。");
            }
            SetWindowTextW(state->issuesButton,
                           (L"错误信息 (" + std::to_wstring(state->issues.size()) + L")").c_str());
            populateAdvice(*state);
            InvalidateRect(state->window, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            KillTimer(window, analysisRefreshTimer);
            state->mailbox->alive.store(false);
            state->mailbox->window.store(nullptr);
            DeleteObject(state->windowBrush);
            DeleteObject(state->surfaceBrush);
            DeleteObject(state->metricFont);
            DeleteObject(state->smallFont);
            delete state;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace ashare
