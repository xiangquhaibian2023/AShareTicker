#include "main_window.h"
#include "market_controller.h"

#include "chart_view.h"
#include "dashboard_view.h"
#include "dialogs.h"
#include "resource_ids.h"
#include "research_view.h"
#include "stock_screener_view.h"
#include "trade_analysis_view.h"
#include "trade_ledger.h"
#include "trading_algorithm.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
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

void setWindowTextIfChanged(HWND window, const std::wstring& text) {
    int length = GetWindowTextLengthW(window);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, current.data(), length + 1);
    current.resize(static_cast<size_t>(length));
    if (current != text) {
        SetWindowTextW(window, text.c_str());
    }
}

void setStatus(AppState& state, const std::wstring& text) {
    setWindowTextIfChanged(state.status, text);
}

bool activeStrategyUsesFastT0Scan(const AppState& state) {
    auto active = std::find_if(state.strategies.begin(), state.strategies.end(),
                               [&](const StrategyConfig& strategy) {
                                   return strategy.id == state.activeStrategyId;
                               });
    return active != state.strategies.end() && active->kind == StrategyKind::T0Intraday;
}

void restoreHoldingTradeLedgers(AppState& state) {
    const std::string date = currentDateToken();
    const std::string displayDate = date.substr(0, 4) + "-" + date.substr(4, 2) + "-" +
                                    date.substr(6, 2);
    bool changed = false;
    for (auto& [symbol, holding] : state.holdings) {
        auto records = tradesFor(state.holdingTrades, symbol, date);

        // 旧版仅保存买卖汇总；升级时转换为两笔兼容成交，之后统一由分笔账本维护。
        if (records.empty() && holding.todayTradeDate == date &&
            (holding.todayBuyQuantity > 0 || holding.todaySellQuantity > 0)) {
            double buyValue = holding.todayBuyPrice * holding.todayBuyQuantity;
            double sellValue = holding.todaySellPrice * holding.todaySellQuantity;
            double totalValue = buyValue + sellValue;
            double buyFees = totalValue > 0.0 ? holding.todayFees * buyValue / totalValue : 0.0;
            if (holding.todayBuyQuantity > 0) {
                TradeRecord buy;
                buy.id = "legacy-" + date + "-" + symbol + "-buy";
                buy.symbol = symbol;
                buy.time = displayDate + " 09:31";
                buy.side = SignalType::Buy;
                buy.quantity = holding.todayBuyQuantity;
                buy.price = holding.todayBuyPrice;
                buy.commission = buyFees;
                state.holdingTrades.push_back(buy);
            }
            if (holding.todaySellQuantity > 0) {
                TradeRecord sell;
                sell.id = "legacy-" + date + "-" + symbol + "-sell";
                sell.symbol = symbol;
                sell.time = displayDate + " 14:59";
                sell.side = SignalType::Sell;
                sell.quantity = holding.todaySellQuantity;
                sell.price = holding.todaySellPrice;
                sell.commission = holding.todayFees - buyFees;
                state.holdingTrades.push_back(sell);
            }
            records = tradesFor(state.holdingTrades, symbol, date);
            changed = true;
        }

        if (records.empty()) continue;
        if (holding.tradeBaseDate != date) {
            long long buyQuantity = 0;
            long long sellQuantity = 0;
            double buyValueAndFees = 0.0;
            for (const auto& trade : records) {
                if (trade.side == SignalType::Buy) {
                    buyQuantity += trade.quantity;
                    buyValueAndFees += trade.price * trade.quantity + tradeFees(trade);
                } else {
                    sellQuantity += trade.quantity;
                }
            }
            holding.tradeBaseDate = date;
            holding.tradeBaseQuantity = holding.quantity - buyQuantity + sellQuantity;
            holding.tradeBaseCost = holding.cost;
            if (holding.tradeBaseQuantity > 0 && buyQuantity > 0) {
                double reconstructed =
                    (holding.cost * static_cast<double>(holding.tradeBaseQuantity + buyQuantity) -
                     buyValueAndFees) /
                    static_cast<double>(holding.tradeBaseQuantity);
                if (std::isfinite(reconstructed) && reconstructed >= 0.0) {
                    holding.tradeBaseCost = reconstructed;
                }
            }
            changed = true;
        }
        std::string error;
        if (!recalculateHoldingFromTrades(holding, state.holdingTrades, date, &error)) {
            logger().write("WARN", "Holding trade restore skipped symbol=" + symbol +
                                       " reason=" + error);
        } else {
            changed = true;
        }
    }
    if (changed) {
        state.holdingStore.save(state.holdings);
        state.holdingTradeStore.save(state.holdingTrades);
    }
}

std::wstring defaultReplayDate() {
    // 默认使用最近一个工作日；法定节假日由用户在加载失败后继续向前选择。
    SYSTEMTIME value{};
    GetLocalTime(&value);
    do {
        FILETIME fileTime{};
        SystemTimeToFileTime(&value, &fileTime);
        ULARGE_INTEGER ticks{};
        ticks.LowPart = fileTime.dwLowDateTime;
        ticks.HighPart = fileTime.dwHighDateTime;
        ticks.QuadPart -= 24ULL * 60ULL * 60ULL * 10000000ULL;
        fileTime.dwLowDateTime = ticks.LowPart;
        fileTime.dwHighDateTime = ticks.HighPart;
        FileTimeToSystemTime(&fileTime, &value);
    } while (value.wDayOfWeek == 0 || value.wDayOfWeek == 6);
    wchar_t buffer[16]{};
    std::swprintf(buffer, std::size(buffer), L"%04u-%02u-%02u",
                  value.wYear, value.wMonth, value.wDay);
    return buffer;
}

void applyTheme(AppState& state) {
    ThemePalette palette = themePalette();
    // 画刷由 AppState 持有；重新应用主题前先释放旧 GDI 对象。
    if (state.windowBrush) {
        DeleteObject(state.windowBrush);
    }
    if (state.surfaceBrush) {
        DeleteObject(state.surfaceBrush);
    }
    if (state.inputBrush) {
        DeleteObject(state.inputBrush);
    }
    if (state.sidebarBrush) {
        DeleteObject(state.sidebarBrush);
    }
    state.windowBrush = CreateSolidBrush(palette.window);
    state.surfaceBrush = CreateSolidBrush(palette.surface);
    state.inputBrush = CreateSolidBrush(palette.surfaceAlt);
    state.sidebarBrush = CreateSolidBrush(palette.sidebar);

    HWND roundedInputs[] = {state.edit, state.quantityEdit, state.costEdit,
                            state.replayDateEdit};
    for (HWND control : roundedInputs) {
        installRoundedInputTheme(control);
    }
    HWND roundedCombos[] = {state.strategyCombo, state.dataModeCombo};
    for (HWND control : roundedCombos) {
        installRoundedComboTheme(control);
    }
    HWND themedLists[] = {state.indexList, state.list, state.signalHistoryList};
    for (HWND control : themedLists) {
        setExplorerControlTheme(control);
    }

    HWND lists[] = {state.indexList, state.list, state.signalHistoryList};
    for (HWND list : lists) {
        ListView_SetBkColor(list, palette.surface);
        ListView_SetTextBkColor(list, palette.surface);
        ListView_SetTextColor(list, palette.text);
        InvalidateRect(ListView_GetHeader(list), nullptr, TRUE);
    }
    setImmersiveDarkTitleBar(state.window);
    RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT drawOwnerButton(AppState& state, const DRAWITEMSTRUCT& item) {
    // 所有主窗口按钮统一自绘，并根据当前页面或 K 线周期计算激活状态。
    ThemePalette palette = themePalette();
    int controlId = static_cast<int>(item.CtlID);
    bool sidebarButton = controlId == IDC_VIEW_FAVORITES || controlId == IDC_VIEW_HOLDINGS ||
                         controlId == IDC_VIEW_STRATEGY || controlId == IDC_VIEW_SCREENER ||
                         controlId == IDC_VIEW_RESEARCH ||
                         controlId == IDC_VIEW_TRADE_ANALYSIS ||
                         controlId == IDC_OPEN_LOGS;
    bool primary = controlId == IDC_SEARCH || controlId == IDC_SAVE_HOLDING ||
                   controlId == IDC_REPLAY_LOAD;
    bool active = (controlId == IDC_DAY_KLINE && state.currentKlt == 101) ||
                  (controlId == IDC_MIN_KLINE && state.currentKlt == 1) ||
                  (controlId == IDC_REPLAY_PLAY && state.replayPlaying) ||
                  (controlId == IDC_VIEW_FAVORITES && state.detailView == DetailView::Favorites) ||
                  (controlId == IDC_VIEW_HOLDINGS && state.detailView == DetailView::Holdings) ||
                  (controlId == IDC_VIEW_STRATEGY && state.detailView == DetailView::Strategy) ||
                  (controlId == IDC_VIEW_SCREENER && state.detailView == DetailView::Screener) ||
                  (controlId == IDC_VIEW_RESEARCH && state.detailView == DetailView::Research) ||
                  (controlId == IDC_VIEW_TRADE_ANALYSIS &&
                   state.detailView == DetailView::TradeAnalysis);
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;

    COLORREF fill = sidebarButton ? palette.sidebar : palette.surface;
    COLORREF border = sidebarButton ? palette.sidebar : palette.border;
    COLORREF textColor = disabled ? palette.muted : palette.text;
    if (active && sidebarButton) {
        fill = palette.selection;
        border = palette.accent;
        textColor = palette.accent;
    } else if (primary || active) {
        fill = palette.accent;
        border = palette.accent;
        textColor = palette.accentText;
    } else if (pressed) {
        fill = palette.selection;
        border = palette.accent;
    }
    if ((item.itemState & ODS_FOCUS) != 0 && !primary && !active) {
        border = palette.accent;
    }

    RECT rect = item.rcItem;
    HBRUSH baseBrush = CreateSolidBrush(sidebarButton ? palette.sidebar : palette.surface);
    FillRect(item.hDC, &rect, baseBrush);
    DeleteObject(baseBrush);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    if (active && sidebarButton) {
        RECT indicator{rect.left, rect.top + 8, rect.left + 3, rect.bottom - 8};
        HBRUSH indicatorBrush = CreateSolidBrush(palette.accent);
        FillRect(item.hDC, &indicator, indicatorBrush);
        DeleteObject(indicatorBrush);
    }

    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    if (pressed) {
        OffsetRect(&rect, 0, 1);
    }
    if (sidebarButton) {
        const wchar_t* icon = L"\xE80F";
        switch (controlId) {
        case IDC_VIEW_HOLDINGS: icon = L"\xE8C7"; break;
        case IDC_VIEW_STRATEGY: icon = L"\xE9D2"; break;
        case IDC_VIEW_SCREENER: icon = L"\xE71E"; break;
        case IDC_VIEW_RESEARCH: icon = L"\xE9D9"; break;
        case IDC_VIEW_TRADE_ANALYSIS: icon = L"\xE9D5"; break;
        case IDC_OPEN_LOGS: icon = L"\xE8A5"; break;
        default: break;
        }
        RECT iconRect = rect;
        iconRect.left += 14;
        iconRect.right = iconRect.left + 24;
        HFONT oldFont = static_cast<HFONT>(SelectObject(item.hDC, state.iconFont));
        DrawTextW(item.hDC, icon, -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item.hDC, oldFont);

        RECT textRect = rect;
        textRect.left += 50;
        textRect.right -= 10;
        oldFont = static_cast<HFONT>(SelectObject(item.hDC, state.sectionFont));
        DrawTextW(item.hDC, text, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item.hDC, oldFont);
    } else {
        HFONT oldFont = static_cast<HFONT>(SelectObject(item.hDC, state.font));
        DrawTextW(item.hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item.hDC, oldFont);
    }
    return TRUE;
}

LRESULT drawListView(AppState& state, NMLVCUSTOMDRAW* customDraw) {
    // 仅接管颜色：行情涨跌、持仓盈亏和策略方向按红涨绿跌规则显示。
    ThemePalette palette = themePalette();
    switch (customDraw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        bool selected = (customDraw->nmcd.uItemState & CDIS_SELECTED) != 0;
        int row = static_cast<int>(customDraw->nmcd.dwItemSpec);
        customDraw->clrTextBk = selected ? palette.selection
                                         : row % 2 == 0 ? palette.surfaceAlt
                                                        : palette.surface;
        customDraw->clrText = palette.text;
        HWND list = customDraw->nmcd.hdr.hwndFrom;
        int column = customDraw->iSubItem;
        bool quoteView = list == state.indexList || state.detailView == DetailView::Favorites;
        if (quoteView && column >= 2 && column <= 4) {
            wchar_t valueText[64]{};
            ListView_GetItemText(list, row, 3, valueText, 64);
            double value = std::wcstod(valueText, nullptr);
            customDraw->clrText = value > 0.0 ? palette.up : value < 0.0 ? palette.down : palette.text;
        } else if (list == state.list && state.detailView == DetailView::Holdings &&
                   column >= 6 && column <= 10) {
            wchar_t valueText[64]{};
            ListView_GetItemText(list, row, column, valueText, 64);
            double value = std::wcstod(valueText, nullptr);
            customDraw->clrText = value > 0.0 ? palette.up : value < 0.0 ? palette.down : palette.text;
        } else if (list == state.list && state.detailView == DetailView::Strategy && column == 4) {
            wchar_t signalText[64]{};
            ListView_GetItemText(list, row, column, signalText, 64);
            std::wstring signal(signalText);
            if (signal.find(L"买入") != std::wstring::npos) {
                customDraw->clrText = palette.up;
            } else if (signal.find(L"卖出") != std::wstring::npos) {
                customDraw->clrText = palette.down;
            }
        } else if (list == state.list && state.detailView == DetailView::Strategy &&
                   column >= 7 && column <= 11) {
            wchar_t valueText[64]{};
            ListView_GetItemText(list, row, column, valueText, 64);
            double value = std::wcstod(valueText, nullptr);
            customDraw->clrText = value > 0.0 ? palette.up : value < 0.0 ? palette.down : palette.text;
        } else if (list == state.signalHistoryList &&
                   ((state.detailView == DetailView::Strategy &&
                     !state.strategyDetailsShowTrades && column == 2) ||
                    ((state.detailView == DetailView::Holdings ||
                      state.strategyDetailsShowTrades) && column == 3))) {
            wchar_t signalText[32]{};
            ListView_GetItemText(list, row, column, signalText, 32);
            customDraw->clrText = std::wstring(signalText) == L"买入" ? palette.up : palette.down;
        } else if (list == state.signalHistoryList &&
                   state.detailView == DetailView::Strategy &&
                   !state.strategyDetailsShowTrades && column == 10) {
            wchar_t valueText[64]{};
            ListView_GetItemText(list, row, column, valueText, 64);
            double value = std::wcstod(valueText, nullptr);
            customDraw->clrText = value > 0.0 ? palette.up : value < 0.0 ? palette.down : palette.text;
        }
        return CDRF_NEWFONT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

constexpr int sidebarWidth = 200;
constexpr int compactToolbarThreshold = 1120;

struct LayoutMetrics {
    // 同一组指标同时用于父窗口面板和子控件，避免背景与控件边界错位。
    int contentLeft = 0;
    int contentRight = 0;
    int contentWidth = 0;
    int toolbarTop = 60;
    int toolbarBottom = 132;
    int chartPanelTop = 144;
    int chartPanelBottom = 0;
    int indexPanelTop = 0;
    int indexLabelTop = 0;
    int indexListTop = 0;
    int indexListHeight = 0;
    int indexPanelBottom = 0;
    int detailPanelTop = 0;
    int detailPanelLeft = 0;
    int detailPanelRight = 0;
    int detailLabelTop = 0;
    int detailListTop = 0;
    int detailListHeight = 0;
    int detailPanelBottom = 0;
    int signalPanelLeft = 0;
    int signalPanelRight = 0;
    int signalPanelTop = 0;
    int signalLabelTop = 0;
    int signalListTop = 0;
    int signalListHeight = 0;
    int signalPanelBottom = 0;
    int primarySplitterTop = 0;
    int primarySplitterBottom = 0;
    int secondarySplitterTop = 0;
    int secondarySplitterBottom = 0;
};

double chartRatioForView(const AppState& state) {
    switch (state.detailView) {
    case DetailView::Favorites: return state.favoritesChartRatio;
    case DetailView::Holdings: return state.holdingsChartRatio;
    case DetailView::Strategy: return state.strategyChartRatio;
    case DetailView::Screener: return 0.55;
    case DetailView::Research: return 0.55;
    case DetailView::TradeAnalysis: return 0.55;
    }
    return 0.55;
}

LayoutMetrics calculateLayoutMetrics(const AppState& state, int width, int height) {
    LayoutMetrics metrics;
    constexpr int margin = 18;
    constexpr int splitterGap = 12;
    const int minimumPanelHeight = state.detailView == DetailView::Strategy ? 120 : 96;
    metrics.contentLeft = sidebarWidth + margin;
    metrics.contentRight = std::max(metrics.contentLeft + 200, width - margin);
    metrics.contentWidth = metrics.contentRight - metrics.contentLeft;
    // 持仓与策略页采用摘要工具栏，统一保持两行高度。
    if (state.detailView == DetailView::Strategy ||
        state.detailView == DetailView::Holdings ||
               metrics.contentWidth < compactToolbarThreshold) {
        metrics.toolbarBottom = 174;
        metrics.chartPanelTop = 186;
    }
    int contentBottom = std::max(metrics.chartPanelTop + 360, height - margin);
    int available = contentBottom - metrics.chartPanelTop;
    bool twoLowerPanels = true;
    int minimumLowerHeight = twoLowerPanels
                                 ? minimumPanelHeight * 2 + splitterGap
                                 : minimumPanelHeight;
    int maximumChartHeight = std::max(140, available - splitterGap - minimumLowerHeight);
    int minimumChartHeight = std::min(220, maximumChartHeight);
    int preferredChartHeight = static_cast<int>(
        std::lround(static_cast<double>(available) * chartRatioForView(state)));
    int chartHeight = std::clamp(preferredChartHeight, minimumChartHeight,
                                 maximumChartHeight);
    metrics.chartPanelBottom = metrics.chartPanelTop + chartHeight;
    metrics.primarySplitterTop = metrics.chartPanelBottom;
    metrics.primarySplitterBottom = metrics.primarySplitterTop + splitterGap;
    metrics.detailPanelLeft = metrics.contentLeft;
    metrics.detailPanelRight = metrics.contentRight;

    if (state.detailView == DetailView::Favorites) {
        metrics.indexPanelTop = metrics.primarySplitterBottom;
        int panelSpace = contentBottom - metrics.indexPanelTop - splitterGap;
        int indexHeight = std::clamp(
            static_cast<int>(std::lround(panelSpace * state.favoritesListRatio)),
            minimumPanelHeight, panelSpace - minimumPanelHeight);
        metrics.indexPanelBottom = metrics.indexPanelTop + indexHeight;
        metrics.indexLabelTop = metrics.indexPanelTop + 10;
        metrics.indexListTop = metrics.indexPanelTop + 34;
        metrics.indexListHeight = std::max(40, indexHeight - 44);
        metrics.secondarySplitterTop = metrics.indexPanelBottom;
        metrics.secondarySplitterBottom = metrics.secondarySplitterTop + splitterGap;
        metrics.detailPanelTop = metrics.secondarySplitterBottom;
        metrics.detailPanelBottom = contentBottom;
    } else if (state.detailView == DetailView::Strategy ||
               state.detailView == DetailView::Holdings) {
        metrics.detailPanelTop = metrics.primarySplitterBottom;
        int panelSpace = contentBottom - metrics.detailPanelTop - splitterGap;
        int observationHeight = std::clamp(
            static_cast<int>(std::lround(panelSpace *
                (state.detailView == DetailView::Strategy
                     ? state.strategyListRatio : state.holdingsListRatio))),
            minimumPanelHeight, panelSpace - minimumPanelHeight);
        metrics.detailPanelBottom = metrics.detailPanelTop + observationHeight;
        metrics.secondarySplitterTop = metrics.detailPanelBottom;
        metrics.secondarySplitterBottom = metrics.secondarySplitterTop + splitterGap;
        metrics.signalPanelLeft = metrics.contentLeft;
        metrics.signalPanelRight = metrics.contentRight;
        metrics.signalPanelTop = metrics.secondarySplitterBottom;
        metrics.signalPanelBottom = contentBottom;
        metrics.signalLabelTop = metrics.signalPanelTop + 10;
        metrics.signalListTop = metrics.signalPanelTop + 34;
        metrics.signalListHeight = std::max(40, metrics.signalPanelBottom - metrics.signalListTop - 10);
    } else {
        metrics.detailPanelTop = metrics.primarySplitterBottom;
        metrics.detailPanelBottom = contentBottom;
    }
    metrics.detailLabelTop = metrics.detailPanelTop + 10;
    metrics.detailListTop = metrics.detailPanelTop + 34;
    metrics.detailListHeight = std::max(40, metrics.detailPanelBottom - metrics.detailListTop - 10);
    return metrics;
}

void drawDashboardPanel(HDC dc, const RECT& rect, const ThemePalette& palette) {
    HBRUSH brush = CreateSolidBrush(palette.surface);
    HPEN pen = CreatePen(PS_SOLID, 1, palette.border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void drawHorizontalSplitter(HDC dc, const LayoutMetrics& metrics, int top, int bottom,
                            bool active, const ThemePalette& palette) {
    int centerY = (top + bottom) / 2;
    int centerX = (metrics.contentLeft + metrics.contentRight) / 2;
    COLORREF color = active ? palette.accent : palette.muted;
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    for (int offset = -2; offset <= 2; offset += 2) {
        MoveToEx(dc, centerX - 24, centerY + offset, nullptr);
        LineTo(dc, centerX + 24, centerY + offset);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void paintDashboardBackground(HDC dc, int width, int height, const AppState& state,
                              const ThemePalette& palette) {
    RECT client{0, 0, width, height};
    HBRUSH windowBrush = CreateSolidBrush(palette.window);
    FillRect(dc, &client, windowBrush);
    DeleteObject(windowBrush);

    RECT sidebar{0, 0, sidebarWidth, height};
    HBRUSH sidebarBrush = CreateSolidBrush(palette.sidebar);
    FillRect(dc, &sidebar, sidebarBrush);
    DeleteObject(sidebarBrush);
    HPEN separator = CreatePen(PS_SOLID, 1, palette.border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, separator));
    MoveToEx(dc, sidebarWidth - 1, 0, nullptr);
    LineTo(dc, sidebarWidth - 1, height);
    SelectObject(dc, oldPen);
    DeleteObject(separator);

    // 算法研究页由独立子窗口完整绘制内容区，父窗口只保留统一侧栏和背景。
    if (state.detailView == DetailView::Screener ||
        state.detailView == DetailView::Research ||
        state.detailView == DetailView::TradeAnalysis) {
        return;
    }

    LayoutMetrics metrics = calculateLayoutMetrics(state, width, height);
    drawDashboardPanel(dc, RECT{metrics.contentLeft, metrics.toolbarTop,
                                metrics.contentRight, metrics.toolbarBottom}, palette);
    drawDashboardPanel(dc, RECT{metrics.contentLeft, metrics.chartPanelTop,
                                metrics.contentRight, metrics.chartPanelBottom}, palette);
    if (state.detailView == DetailView::Favorites) {
        drawDashboardPanel(dc, RECT{metrics.contentLeft, metrics.indexPanelTop,
                                    metrics.contentRight, metrics.indexPanelBottom}, palette);
    }
    drawDashboardPanel(dc, RECT{metrics.detailPanelLeft, metrics.detailPanelTop,
                                metrics.detailPanelRight, metrics.detailPanelBottom}, palette);
    if (state.detailView == DetailView::Strategy || state.detailView == DetailView::Holdings) {
        drawDashboardPanel(dc, RECT{metrics.signalPanelLeft, metrics.signalPanelTop,
                                    metrics.signalPanelRight, metrics.signalPanelBottom}, palette);
    }
    drawHorizontalSplitter(dc, metrics, metrics.primarySplitterTop,
                           metrics.primarySplitterBottom,
                           state.activeSplitter == DashboardSplitter::ChartAndLists, palette);
    drawHorizontalSplitter(dc, metrics, metrics.secondarySplitterTop,
                           metrics.secondarySplitterBottom,
                           state.activeSplitter == DashboardSplitter::ListAndList, palette);
}

DashboardSplitter hitTestSplitter(const AppState& state, int x, int y,
                                   int width, int height) {
    if (state.detailView == DetailView::Screener ||
        state.detailView == DetailView::Research ||
        state.detailView == DetailView::TradeAnalysis) {
        return DashboardSplitter::None;
    }
    LayoutMetrics metrics = calculateLayoutMetrics(state, width, height);
    if (x < metrics.contentLeft || x >= metrics.contentRight) {
        return DashboardSplitter::None;
    }
    if (y >= metrics.primarySplitterTop && y < metrics.primarySplitterBottom) {
        return DashboardSplitter::ChartAndLists;
    }
    if (y >= metrics.secondarySplitterTop && y < metrics.secondarySplitterBottom) {
        return DashboardSplitter::ListAndList;
    }
    return DashboardSplitter::None;
}

void updateSplitterRatio(AppState& state, int mouseY, int width, int height) {
    LayoutMetrics metrics = calculateLayoutMetrics(state, width, height);
    if (state.activeSplitter == DashboardSplitter::ChartAndLists) {
        int contentBottom = (state.detailView == DetailView::Strategy ||
                             state.detailView == DetailView::Holdings)
                                ? metrics.signalPanelBottom
                                : metrics.detailPanelBottom;
        int available = contentBottom - metrics.chartPanelTop;
        double ratio = available > 0
                           ? static_cast<double>(mouseY - metrics.chartPanelTop) / available
                           : 0.5;
        ratio = std::clamp(ratio, 0.05, 0.95);
        switch (state.detailView) {
        case DetailView::Favorites: state.favoritesChartRatio = ratio; break;
        case DetailView::Holdings: state.holdingsChartRatio = ratio; break;
        case DetailView::Strategy: state.strategyChartRatio = ratio; break;
        case DetailView::Screener: break;
        case DetailView::Research: break;
        case DetailView::TradeAnalysis: break;
        }
    } else if (state.activeSplitter == DashboardSplitter::ListAndList) {
        int panelTop = state.detailView == DetailView::Favorites
                           ? metrics.indexPanelTop
                           : metrics.detailPanelTop;
        int panelBottom = state.detailView == DetailView::Favorites
                              ? metrics.detailPanelBottom
                              : metrics.signalPanelBottom;
        int available = panelBottom - panelTop - 12;
        double ratio = available > 0
                           ? static_cast<double>(mouseY - panelTop) / available
                           : 0.5;
        ratio = std::clamp(ratio, 0.05, 0.95);
        if (state.detailView == DetailView::Favorites) {
            state.favoritesListRatio = ratio;
        } else if (state.detailView == DetailView::Strategy) {
            state.strategyListRatio = ratio;
        } else if (state.detailView == DetailView::Holdings) {
            state.holdingsListRatio = ratio;
        }
    }
}

void layout(AppState& state, int width, int height) {
    // 布局只计算位置，不创建控件；窗口缩放和页面切换都复用该函数。
    constexpr int buttonH = 36;
    constexpr int gap = 10;
    LayoutMetrics metrics = calculateLayoutMetrics(state, width, height);
    MoveWindow(state.title, 18, 17, sidebarWidth - 36, 38, TRUE);
    constexpr int headerGap = 8;
    bool standaloneView = state.detailView == DetailView::Screener ||
                          state.detailView == DetailView::Research ||
                          state.detailView == DetailView::TradeAnalysis;
    int sourceControlsWidth = standaloneView ? 0 : 184;
    if (state.dataMode == MarketDataMode::Replay &&
        !standaloneView) {
        sourceControlsWidth = state.detailView == DetailView::Strategy ? 556 : 382;
    }
    int sourceX = metrics.contentRight - sourceControlsWidth;
    MoveWindow(state.pageTitle, metrics.contentLeft + 2, 17,
               std::max(120, sourceX - metrics.contentLeft - 12), 34, TRUE);
    MoveWindow(state.dataModeLabel, sourceX, 19, 52, 26, TRUE);
    sourceX += 52 + headerGap;
    MoveWindow(state.dataModeCombo, sourceX, 13, 124, 220, TRUE);
    sourceX += 124 + headerGap;
    if (state.dataMode == MarketDataMode::Replay) {
        MoveWindow(state.replayDateLabel, sourceX, 19, 32, 26, TRUE);
        sourceX += 32 + headerGap;
        MoveWindow(state.replayDateEdit, sourceX, 13, 96, 32, TRUE);
        sourceX += 96 + headerGap;
        MoveWindow(GetDlgItem(state.window, IDC_REPLAY_LOAD), sourceX, 13, 52, 32, TRUE);
        if (state.detailView == DetailView::Strategy) {
            sourceX += 52 + headerGap;
            MoveWindow(GetDlgItem(state.window, IDC_REPLAY_PLAY), sourceX, 13, 52, 32, TRUE);
            sourceX += 52 + headerGap;
            MoveWindow(GetDlgItem(state.window, IDC_REPLAY_STEP), sourceX, 13, 52, 32, TRUE);
            sourceX += 52 + headerGap;
            MoveWindow(GetDlgItem(state.window, IDC_REPLAY_RESET), sourceX, 13, 52, 32, TRUE);
        }
    }
    MoveWindow(GetDlgItem(state.window, IDC_VIEW_FAVORITES), 10, 80, sidebarWidth - 20, 46, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_VIEW_HOLDINGS), 10, 134, sidebarWidth - 20, 46, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_VIEW_STRATEGY), 10, 188, sidebarWidth - 20, 46, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_VIEW_SCREENER), 10, 242, sidebarWidth - 20, 46, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_VIEW_RESEARCH), 10, 296, sidebarWidth - 20, 46, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_VIEW_TRADE_ANALYSIS), 10, 350,
               sidebarWidth - 20, 46, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_OPEN_LOGS), 10, std::max(250, height - 62), sidebarWidth - 20, 46, TRUE);

    if (standaloneView) {
        HWND target = state.detailView == DetailView::Screener
                          ? state.screenerView
                          : state.detailView == DetailView::Research
                                ? state.researchView : state.tradeAnalysisView;
        SetWindowPos(target, nullptr, metrics.contentLeft, 60,
                     metrics.contentWidth, std::max(200, height - 78),
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        InvalidateRect(target, nullptr, FALSE);
        return;
    }

    int toolbarTop = metrics.toolbarTop + 10;
    bool compactToolbar = metrics.contentWidth < compactToolbarThreshold;
    bool multiRowToolbar = state.detailView != DetailView::Favorites || compactToolbar;
    int actionTop = multiRowToolbar ? toolbarTop + 46 : toolbarTop;
    int minuteWidth = 84;
    int dayWidth = 64;
    int minuteX = metrics.contentRight - 12 - minuteWidth;
    int periodLeft = minuteX - gap - dayWidth;
    int x = metrics.contentLeft + 12;
    if (state.detailView == DetailView::Favorites) {
        MoveWindow(state.codeLabel, x, toolbarTop + 7, 40, 22, TRUE);
        x += 40 + gap;
        MoveWindow(state.edit, x, toolbarTop, 140, buttonH, TRUE);
        x += 140 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_SEARCH), x, toolbarTop, 68, buttonH, TRUE);
        x += 68 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_ADD), x, toolbarTop, 60, buttonH, TRUE);
        x = multiRowToolbar ? metrics.contentLeft + 12 : x + 60 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_DELETE), x, actionTop, 60, buttonH, TRUE);
        x += 60 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_REFRESH), x, actionTop, 60, buttonH, TRUE);
        x += 60 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_TOGGLE_STRATEGY), x, actionTop, 84, buttonH, TRUE);
    } else if (state.detailView == DetailView::Holdings) {
        int holdingGap = compactToolbar ? 8 : 10;
        int editWidth = compactToolbar ? 84 : 96;
        int tradesWidth = compactToolbar ? 72 : 90;
        int fundsWidth = compactToolbar ? 64 : 82;
        int deleteWidth = compactToolbar ? 72 : 90;
        int refreshWidth = compactToolbar ? 58 : 68;
        SetWindowTextW(GetDlgItem(state.window, IDC_TODAY_TRADES),
                       compactToolbar ? L"成交" : L"成交管理");
        SetWindowTextW(GetDlgItem(state.window, IDC_ACCOUNT_FUNDS),
                       compactToolbar ? L"资金" : L"资金账户");
        SetWindowTextW(GetDlgItem(state.window, IDC_DELETE),
                       compactToolbar ? L"删除" : L"删除持仓");
        int summaryWidth = std::min(compactToolbar ? 390 : 560,
                                    std::max(180, periodLeft - x - editWidth - gap - 14));
        MoveWindow(state.holdingSummary, x, toolbarTop + 7, summaryWidth, 24, TRUE);
        MoveWindow(GetDlgItem(state.window, IDC_SAVE_HOLDING),
                   x + summaryWidth + gap, toolbarTop, editWidth, buttonH, TRUE);
        x = metrics.contentLeft + 12;
        MoveWindow(GetDlgItem(state.window, IDC_TODAY_TRADES), x, actionTop,
                   tradesWidth, buttonH, TRUE);
        x += tradesWidth + holdingGap;
        MoveWindow(GetDlgItem(state.window, IDC_ACCOUNT_FUNDS), x, actionTop,
                   fundsWidth, buttonH, TRUE);
        x += fundsWidth + holdingGap;
        MoveWindow(GetDlgItem(state.window, IDC_DELETE), x, actionTop,
                   deleteWidth, buttonH, TRUE);
        x += deleteWidth + holdingGap;
        MoveWindow(GetDlgItem(state.window, IDC_REFRESH), x, actionTop,
                   refreshWidth, buttonH, TRUE);
    } else {
        int toolbarGap = compactToolbar ? 8 : 10;
        MoveWindow(state.strategyLabel, x, toolbarTop + 7, 72, 22, TRUE);
        x += 72 + gap;
        int strategyComboWidth = compactToolbar ? 166 : 190;
        int managerWidth = compactToolbar ? 82 : 94;
        MoveWindow(state.strategyCombo, x, toolbarTop, strategyComboWidth, 240, TRUE);
        x += strategyComboWidth + gap;
        MoveWindow(GetDlgItem(state.window, IDC_MANAGE_STRATEGIES), x, toolbarTop,
                   managerWidth, buttonH, TRUE);
        x += managerWidth + 22;
        int editWidth = compactToolbar ? 84 : 96;
        int summaryWidth = std::min(compactToolbar ? 300 : 480,
                                    std::max(140, periodLeft - x - editWidth - toolbarGap - 14));
        MoveWindow(state.holdingSummary, x, toolbarTop + 7, summaryWidth, 24, TRUE);
        MoveWindow(GetDlgItem(state.window, IDC_SAVE_HOLDING),
                   x + summaryWidth + toolbarGap, toolbarTop, editWidth, buttonH, TRUE);
        x = metrics.contentLeft + 12;
        MoveWindow(GetDlgItem(state.window, IDC_TODAY_TRADES), x, actionTop, 90, buttonH, TRUE);
        x += 90 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_DETAIL_TOGGLE), x, actionTop, 90, buttonH, TRUE);
        x += 90 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_REFRESH), x, actionTop, 72, buttonH, TRUE);
        x += 72 + gap;
        MoveWindow(GetDlgItem(state.window, IDC_DELETE), x, actionTop, 90, buttonH, TRUE);
    }

    MoveWindow(GetDlgItem(state.window, IDC_MIN_KLINE), minuteX, toolbarTop, minuteWidth, buttonH, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_DAY_KLINE), minuteX - gap - dayWidth, toolbarTop, dayWidth, buttonH, TRUE);
    if (state.detailView == DetailView::Strategy) {
        int statusLeft = metrics.contentLeft + 14 + 90 + gap + 90 + gap + 72 + gap + 90 + 16;
        MoveWindow(state.status, statusLeft, actionTop + 8,
                   std::max(120, metrics.contentRight - statusLeft - 14), 20, TRUE);
    } else if (state.detailView == DetailView::Holdings) {
        int statusLeft = metrics.contentLeft + 14 + 90 + gap + 82 + gap + 90 + gap + 68 + 16;
        MoveWindow(state.status, statusLeft, actionTop + 8,
                   std::max(120, metrics.contentRight - statusLeft - 14), 20, TRUE);
    } else {
        int statusTop = multiRowToolbar ? actionTop + buttonH + 4 : toolbarTop + buttonH + 5;
        MoveWindow(state.status, metrics.contentLeft + 14, statusTop,
                   metrics.contentWidth - 28, 20, TRUE);
    }

    if (state.detailView == DetailView::Favorites) {
        MoveWindow(state.indexLabel, metrics.contentLeft + 14, metrics.indexLabelTop,
                   metrics.contentWidth - 28, 22, TRUE);
        MoveWindow(state.indexList, metrics.contentLeft + 10, metrics.indexListTop,
                   metrics.contentWidth - 20, metrics.indexListHeight, TRUE);
    }

    MoveWindow(state.favoritesLabel, metrics.detailPanelLeft + 14, metrics.detailLabelTop,
               metrics.detailPanelRight - metrics.detailPanelLeft - 28, 22, TRUE);
    MoveWindow(state.list, metrics.detailPanelLeft + 10, metrics.detailListTop,
               metrics.detailPanelRight - metrics.detailPanelLeft - 20,
               metrics.detailListHeight, TRUE);
    if (state.detailView == DetailView::Strategy || state.detailView == DetailView::Holdings) {
        MoveWindow(state.signalHistoryLabel, metrics.signalPanelLeft + 14, metrics.signalLabelTop,
                   metrics.signalPanelRight - metrics.signalPanelLeft - 28, 22, TRUE);
        MoveWindow(state.signalHistoryList, metrics.signalPanelLeft + 10, metrics.signalListTop,
                   metrics.signalPanelRight - metrics.signalPanelLeft - 20,
                   metrics.signalListHeight, TRUE);
    }

    // NOCOPYBITS 丢弃尺寸变化前的图表像素，防止旧日期轴在新高度处残留。
    int technicalWidth = state.detailView == DetailView::Strategy
                             ? std::clamp(metrics.contentWidth * 2 / 5, 350, 460) : 0;
    if (technicalWidth > 0) {
        int left = metrics.contentRight - technicalWidth;
        MoveWindow(state.technicalSwingLabel, left + 12, metrics.chartPanelTop + 14,
                   technicalWidth - 144, 24, TRUE);
        MoveWindow(GetDlgItem(state.window, IDC_TECHNICAL_SWING_TOGGLE),
                   metrics.contentRight - 128, metrics.chartPanelTop + 8, 112, 32, TRUE);
        MoveWindow(state.technicalSwingDetails, left + 12, metrics.chartPanelTop + 48,
                   technicalWidth - 28, std::max(60, metrics.chartPanelBottom - metrics.chartPanelTop - 60), TRUE);
    }
    SetWindowPos(state.kline, nullptr, metrics.contentLeft + 10, metrics.chartPanelTop + 10,
                 metrics.contentWidth - 20 - technicalWidth,
                 std::max(120, metrics.chartPanelBottom - metrics.chartPanelTop - 20),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    UINT redrawFlags = RDW_INVALIDATE | RDW_ERASE;
    if (state.activeSplitter == DashboardSplitter::None) {
        redrawFlags |= RDW_UPDATENOW;
    }
    RedrawWindow(state.kline, nullptr, nullptr, redrawFlags);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CREATE) {
        // AppState 在 WM_CREATE 创建并挂到主窗口，在 WM_DESTROY 统一释放。
        auto* state = new AppState();
        state->instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        state->window = hwnd;
        state->strategyMailbox->window.store(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        // 字体和画刷只创建一次，后续自绘直接复用，避免刷新时频繁分配 GDI 资源。
        state->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        state->titleFont = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        state->sectionFont = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                         DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        state->iconFont = CreateFontW(-19, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
        state->chartAxisFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        state->chartValueFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                            DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

        state->title = CreateWindowW(L"STATIC", L"A股行情", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     hwnd, reinterpret_cast<HMENU>(IDC_TITLE), state->instance, nullptr);
        state->pageTitle = CreateWindowW(L"STATIC", L"行情总览", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                         hwnd, reinterpret_cast<HMENU>(IDC_PAGE_TITLE), state->instance, nullptr);
        state->codeLabel = CreateWindowW(L"STATIC", L"代码", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                         hwnd, reinterpret_cast<HMENU>(IDC_CODE_LABEL), state->instance, nullptr);
        state->quantityLabel = CreateWindowW(L"STATIC", L"期初数量", WS_CHILD, 0, 0, 0, 0,
                                             hwnd, reinterpret_cast<HMENU>(IDC_QUANTITY_LABEL), state->instance, nullptr);
        state->costLabel = CreateWindowW(L"STATIC", L"期初成本", WS_CHILD, 0, 0, 0, 0,
                                         hwnd, reinterpret_cast<HMENU>(IDC_COST_LABEL), state->instance, nullptr);
        state->strategyLabel = CreateWindowW(L"STATIC", L"策略类型", WS_CHILD, 0, 0, 0, 0,
                                             hwnd, reinterpret_cast<HMENU>(IDC_STRATEGY_LABEL), state->instance, nullptr);
        state->dataModeLabel = CreateWindowW(L"STATIC", L"数据源", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                             hwnd, reinterpret_cast<HMENU>(IDC_DATA_MODE_LABEL), state->instance, nullptr);
        state->replayDateLabel = CreateWindowW(L"STATIC", L"日期", WS_CHILD, 0, 0, 0, 0,
                                               hwnd, reinterpret_cast<HMENU>(IDC_REPLAY_DATE_LABEL), state->instance, nullptr);
        state->edit = CreateWindowExW(0, L"EDIT", L"600000",
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_CENTER |
                                          ES_MULTILINE,
                                      0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CODE), state->instance, nullptr);
        state->quantityEdit = CreateWindowExW(0, L"EDIT", L"",
                                               WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER | ES_CENTER |
                                                   ES_MULTILINE,
                                              0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_HOLDING_QUANTITY),
                                              state->instance, nullptr);
        state->costEdit = CreateWindowExW(0, L"EDIT", L"",
                                          WS_CHILD | ES_AUTOHSCROLL | ES_CENTER | ES_MULTILINE,
                                          0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_HOLDING_COST),
                                          state->instance, nullptr);
        state->replayDateEdit = CreateWindowExW(0, L"EDIT", defaultReplayDate().c_str(),
                                                 WS_CHILD | ES_AUTOHSCROLL | ES_CENTER |
                                                     ES_MULTILINE,
                                                0, 0, 0, 0, hwnd,
                                                reinterpret_cast<HMENU>(IDC_REPLAY_DATE),
                                                state->instance, nullptr);
        SendMessageW(state->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
        SendMessageW(state->quantityEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        SendMessageW(state->costEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        SendMessageW(state->replayDateEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        SendMessageW(state->edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如 600000"));
        SendMessageW(state->quantityEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如 1000"));
        SendMessageW(state->costEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"例如 12.345"));
        SendMessageW(state->replayDateEdit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"YYYY-MM-DD"));
        state->strategyCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                               WS_CHILD | WS_VSCROLL | CBS_DROPDOWNLIST |
                                                   CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                               0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_STRATEGY_COMBO),
                                               state->instance, nullptr);
        state->dataModeCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                               WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                                   CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                                   CBS_HASSTRINGS,
                                               0, 0, 0, 0, hwnd,
                                               reinterpret_cast<HMENU>(IDC_DATA_MODE_COMBO),
                                               state->instance, nullptr);
        SendMessageW(state->dataModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"实盘行情"));
        SendMessageW(state->dataModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"行情回放"));
        SendMessageW(state->dataModeCombo, CB_SETCURSEL, 0, 0);
        auto createButton = [&](const wchar_t* text, int id) {
            return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                 0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(id), state->instance, nullptr);
        };
        createButton(L"查询", IDC_SEARCH);
        createButton(L"加入", IDC_ADD);
        createButton(L"删除", IDC_DELETE);
        createButton(L"刷新", IDC_REFRESH);
        createButton(L"保存持仓", IDC_SAVE_HOLDING);
        createButton(L"当日成交", IDC_TODAY_TRADES);
        createButton(L"资金账户", IDC_ACCOUNT_FUNDS);
        createButton(L"成交明细", IDC_DETAIL_TOGGLE);
        createButton(L"开启技术分析", IDC_TECHNICAL_SWING_TOGGLE);
        createButton(L"策略观察", IDC_TOGGLE_STRATEGY);
        createButton(L"策略管理", IDC_MANAGE_STRATEGIES);
        createButton(L"日 K", IDC_DAY_KLINE);
        createButton(L"分钟走势", IDC_MIN_KLINE);
        createButton(L"加载", IDC_REPLAY_LOAD);
        createButton(L"播放", IDC_REPLAY_PLAY);
        createButton(L"单步", IDC_REPLAY_STEP);
        createButton(L"重置", IDC_REPLAY_RESET);
        createButton(L"行情总览", IDC_VIEW_FAVORITES);
        createButton(L"持仓管理", IDC_VIEW_HOLDINGS);
        createButton(L"策略观察", IDC_VIEW_STRATEGY);
        createButton(L"策略选股", IDC_VIEW_SCREENER);
        createButton(L"算法研究", IDC_VIEW_RESEARCH);
        createButton(L"交易分析", IDC_VIEW_TRADE_ANALYSIS);
        createButton(L"运行日志", IDC_OPEN_LOGS);
        state->status = CreateWindowW(L"STATIC", L"正在初始化...", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                      hwnd, reinterpret_cast<HMENU>(IDC_STATUS), state->instance, nullptr);
        state->holdingSummary = CreateWindowW(
            L"STATIC", L"尚未选择持仓", WS_CHILD, 0, 0, 0, 0,
            hwnd, reinterpret_cast<HMENU>(IDC_HOLDING_SUMMARY), state->instance, nullptr);
        state->indexLabel = CreateWindowW(L"STATIC", L"主要指数", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                           hwnd, reinterpret_cast<HMENU>(IDC_INDEX_LABEL), state->instance, nullptr);
        state->indexList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                           WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                           0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_INDEX_LIST), state->instance, nullptr);
        state->favoritesLabel = CreateWindowW(L"STATIC", L"自选行情", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                               hwnd, reinterpret_cast<HMENU>(IDC_FAVORITES_LABEL), state->instance, nullptr);
        state->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                      0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST), state->instance, nullptr);
        state->signalHistoryLabel = CreateWindowW(L"STATIC", L"交易信号明细  等待交易信号", WS_CHILD,
                                                   0, 0, 0, 0, hwnd,
                                                   reinterpret_cast<HMENU>(IDC_SIGNAL_HISTORY_LABEL),
                                                   state->instance, nullptr);
        state->signalHistoryList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                                    WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
                                                    0, 0, 0, 0, hwnd,
                                                    reinterpret_cast<HMENU>(IDC_SIGNAL_HISTORY_LIST),
                                                    state->instance, nullptr);
        state->technicalSwingLabel = CreateWindowW(L"STATIC", L"技术面高抛低吸", WS_CHILD,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TECHNICAL_SWING_LABEL), state->instance, nullptr);
        state->technicalSwingDetails = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TECHNICAL_SWING_DETAILS), state->instance, nullptr);
        setExplorerControlTheme(state->technicalSwingDetails);
        SendMessageW(state->technicalSwingDetails, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 4));
        SendMessageW(state->technicalSwingDetails, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->technicalSwingLabel, WM_SETFONT, reinterpret_cast<WPARAM>(state->sectionFont), TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_TECHNICAL_SWING_TOGGLE), WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        state->kline = CreateWindowExW(0, L"AShareKLineView", L"",
                                       WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                       hwnd, reinterpret_cast<HMENU>(IDC_KLINE), state->instance, nullptr);
        state->screenerView = CreateWindowExW(
            0, L"AShareStockScreenerView", L"", WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwnd, nullptr, state->instance, state);
        state->researchView = CreateWindowExW(
            0, L"AShareResearchView", L"", WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwnd, nullptr, state->instance, state);
        state->tradeAnalysisView = CreateWindowExW(
            0, L"AShareTradeAnalysisView", L"", WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 0, 0, hwnd, nullptr, state->instance, state);

        HWND controls[] = {state->edit, state->quantityEdit, state->costEdit, state->strategyCombo,
                           state->dataModeLabel, state->dataModeCombo,
                           state->replayDateLabel, state->replayDateEdit,
                           state->codeLabel, state->quantityLabel, state->costLabel, state->strategyLabel,
                           state->status, state->holdingSummary, state->indexList,
                           state->list, state->signalHistoryList, state->kline,
                           state->screenerView, state->researchView, state->tradeAnalysisView,
                           GetDlgItem(hwnd, IDC_SEARCH), GetDlgItem(hwnd, IDC_ADD), GetDlgItem(hwnd, IDC_DELETE),
                           GetDlgItem(hwnd, IDC_REFRESH),
                            GetDlgItem(hwnd, IDC_SAVE_HOLDING), GetDlgItem(hwnd, IDC_TODAY_TRADES),
                            GetDlgItem(hwnd, IDC_ACCOUNT_FUNDS), GetDlgItem(hwnd, IDC_DETAIL_TOGGLE),
                           GetDlgItem(hwnd, IDC_TOGGLE_STRATEGY), GetDlgItem(hwnd, IDC_MANAGE_STRATEGIES),
                           GetDlgItem(hwnd, IDC_DAY_KLINE), GetDlgItem(hwnd, IDC_MIN_KLINE),
                           GetDlgItem(hwnd, IDC_REPLAY_LOAD), GetDlgItem(hwnd, IDC_REPLAY_PLAY),
                           GetDlgItem(hwnd, IDC_REPLAY_STEP), GetDlgItem(hwnd, IDC_REPLAY_RESET),
                           GetDlgItem(hwnd, IDC_VIEW_FAVORITES), GetDlgItem(hwnd, IDC_VIEW_HOLDINGS),
                           GetDlgItem(hwnd, IDC_VIEW_STRATEGY), GetDlgItem(hwnd, IDC_VIEW_SCREENER),
                           GetDlgItem(hwnd, IDC_VIEW_RESEARCH),
                           GetDlgItem(hwnd, IDC_VIEW_TRADE_ANALYSIS),
                           GetDlgItem(hwnd, IDC_OPEN_LOGS)};
        HWND sectionControls[] = {state->indexLabel};
        SendMessageW(state->title, WM_SETFONT, reinterpret_cast<WPARAM>(state->titleFont), TRUE);
        SendMessageW(state->pageTitle, WM_SETFONT, reinterpret_cast<WPARAM>(state->titleFont), TRUE);
        for (HWND control : sectionControls) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->sectionFont), TRUE);
        }
        for (HWND control : controls) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        }
        SendMessageW(state->favoritesLabel, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        SendMessageW(state->signalHistoryLabel, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
        ListView_SetExtendedListViewStyle(state->indexList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetExtendedListViewStyle(state->signalHistoryList,
                                          LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        setQuoteColumns(state->indexList);
        configureDetailColumns(*state);
        configureSignalHistoryColumns(*state);
        SetWindowSubclass(ListView_GetHeader(state->indexList), HeaderProc, 1, reinterpret_cast<DWORD_PTR>(state));
        SetWindowSubclass(ListView_GetHeader(state->list), HeaderProc, 2, reinterpret_cast<DWORD_PTR>(state));
        SetWindowSubclass(ListView_GetHeader(state->signalHistoryList), HeaderProc, 3,
                          reinterpret_cast<DWORD_PTR>(state));
        applyTheme(*state);

        // 先恢复本地数据并修正无效关联，再发起第一次网络刷新。
        state->favorites = state->store.load();
        state->holdings = state->holdingStore.load();
        state->holdingTrades = state->holdingTradeStore.load();
        state->strategyTrades = state->strategyTradeStore.load();
        state->accountFunds = state->accountFundsStore.load();
        restoreHoldingTradeLedgers(*state);
        state->holdingStore.save(state->holdings);
        state->strategySymbols = state->strategyStore.load();
        state->strategies = state->strategyConfigStore.load();
        state->activeStrategyId = state->strategyConfigStore.loadActive();
        auto activeStrategy = std::find_if(state->strategies.begin(), state->strategies.end(),
                                           [&](const StrategyConfig& config) {
                                               return config.id == state->activeStrategyId;
                                           });
        if (activeStrategy == state->strategies.end()) {
            state->activeStrategyId = state->strategies.front().id;
        }
        state->strategyConfigStore.save(state->strategies);
        state->strategyConfigStore.saveActive(state->activeStrategyId);
        populateStrategyCombo(*state);
        refreshResearchConfiguration(*state);
        restoreTodayStrategySignals(*state);
        for (auto iterator = state->strategySymbols.begin(); iterator != state->strategySymbols.end();) {
            if (state->favorites.find(*iterator) == state->favorites.end()) {
                iterator = state->strategySymbols.erase(iterator);
            } else {
                ++iterator;
            }
        }
        while (state->strategySymbols.size() > 10) {
            state->strategySymbols.erase(std::prev(state->strategySymbols.end()));
        }
        state->strategyStore.save(state->strategySymbols);
        updateToolbarVisibility(*state);
        state->currentSymbol = majorIndexSymbols().front();
        state->currentKlt = 1;
        state->favoritesChartSelection = {state->currentSymbol, state->currentKlt};
        logger().write("INFO", "Application started favorites=" + std::to_string(state->favorites.size()) +
                                   " holdings=" + std::to_string(state->holdings.size()) +
                                   " strategySymbols=" + std::to_string(state->strategySymbols.size()));
        try {
            refreshDashboard(*state, true);
            state->updatingLists = true;
            selectSymbolInList(state->indexList, state->currentSymbol);
            state->updatingLists = false;
            setStatus(*state, L"指数、自选与持仓行情已加载。");
        } catch (const std::exception& ex) {
            logger().write("WARN", std::string("Initial refresh failed: ") + ex.what());
            setStatus(*state, L"初始化行情暂时失败，将自动重试：" + utf8ToWide(ex.what()));
        }
        startStrategyScan(*state);
        SetTimer(hwnd, REFRESH_TIMER, 5000, nullptr);
        return 0;
    }

    AppState* state = app(hwnd);
    switch (msg) {
    case WM_ERASEBKGND:
        if (state && state->windowBrush) {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            paintDashboardBackground(reinterpret_cast<HDC>(wParam), rect.right, rect.bottom,
                                     *state,
                                     themePalette());
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (state) {
            ThemePalette palette = themePalette();
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            int controlId = GetDlgCtrlID(control);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, controlId == IDC_STATUS ? palette.muted : palette.text);
            if (controlId == IDC_TECHNICAL_SWING_DETAILS) {
                SetBkColor(dc, palette.surface);
                return reinterpret_cast<LRESULT>(state->surfaceBrush);
            }
            if (controlId == IDC_TITLE) {
                return reinterpret_cast<LRESULT>(state->sidebarBrush);
            }
            if (controlId == IDC_STATUS || controlId == IDC_INDEX_LABEL ||
                controlId == IDC_FAVORITES_LABEL || controlId == IDC_SIGNAL_HISTORY_LABEL ||
                controlId == IDC_HOLDING_SUMMARY || controlId == IDC_TECHNICAL_SWING_LABEL ||
                controlId == IDC_CODE_LABEL ||
                controlId == IDC_QUANTITY_LABEL || controlId == IDC_COST_LABEL ||
                controlId == IDC_STRATEGY_LABEL) {
                return reinterpret_cast<LRESULT>(state->surfaceBrush);
            }
            return reinterpret_cast<LRESULT>(state->windowBrush);
        }
        break;
    case WM_CTLCOLOREDIT:
        if (state) {
            ThemePalette palette = themePalette();
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, palette.surfaceAlt);
            SetTextColor(dc, palette.text);
            return reinterpret_cast<LRESULT>(state->inputBrush);
        }
        break;
    case WM_DRAWITEM:
        if (state) {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                return drawOwnerButton(*state, *item);
            }
            if (item->CtlType == ODT_COMBOBOX) {
                return drawThemedComboItem(*item);
            }
        }
        break;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = POINT{1080, 720};
        return 0;
    case WM_SIZE:
        if (state) {
            layout(*state, LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_SETCURSOR:
        if (state && LOWORD(lParam) == HTCLIENT) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd, &point);
            RECT client{};
            GetClientRect(hwnd, &client);
            if (state->activeSplitter != DashboardSplitter::None ||
                hitTestSplitter(*state, point.x, point.y, client.right, client.bottom) !=
                    DashboardSplitter::None) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN:
        if (state) {
            int x = static_cast<short>(LOWORD(lParam));
            int y = static_cast<short>(HIWORD(lParam));
            RECT client{};
            GetClientRect(hwnd, &client);
            DashboardSplitter splitter = hitTestSplitter(*state, x, y,
                                                          client.right, client.bottom);
            if (splitter != DashboardSplitter::None) {
                state->activeSplitter = splitter;
                SetCapture(hwnd);
                SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (state && state->activeSplitter != DashboardSplitter::None &&
            GetCapture() == hwnd) {
            int y = static_cast<short>(HIWORD(lParam));
            RECT client{};
            GetClientRect(hwnd, &client);
            updateSplitterRatio(*state, y, client.right, client.bottom);
            layout(*state, client.right, client.bottom);
            InvalidateRect(hwnd, nullptr, TRUE);
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (state && state->activeSplitter != DashboardSplitter::None) {
            state->activeSplitter = DashboardSplitter::None;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            InvalidateRect(state->kline, nullptr, FALSE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (state && state->activeSplitter != DashboardSplitter::None) {
            state->activeSplitter = DashboardSplitter::None;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_TIMER:
        if (state && wParam == REFRESH_TIMER && state->dataMode == MarketDataMode::Live &&
            state->detailView != DetailView::Screener &&
            state->detailView != DetailView::Research &&
            state->detailView != DetailView::TradeAnalysis) {
            // 行情每 5 秒刷新；分钟图同步刷新，日 K 仅在缺失时自动重新加载。
            try {
                refreshDashboard(*state, state->currentKlt == 1 || state->kLines.empty());
            } catch (const std::exception& ex) {
                logger().write("WARN", std::string("Automatic refresh failed: ") + ex.what());
                setStatus(*state, L"自动刷新暂时失败，将继续重试：" + utf8ToWide(ex.what()));
            }
            ++state->refreshTicks;
            // T0 跟随每个 5 秒行情周期扫描；其他策略维持约 60 秒一次。
            if (activeStrategyUsesFastT0Scan(*state) || state->refreshTicks >= 12) {
                state->refreshTicks = 0;
                startStrategyScan(*state);
            }
        } else if (state && wParam == REPLAY_TIMER) {
            try {
                advanceReplay(*state);
            } catch (const std::exception& ex) {
                state->replayPlaying = false;
                KillTimer(hwnd, REPLAY_TIMER);
                SetWindowTextW(GetDlgItem(hwnd, IDC_REPLAY_PLAY), L"播放");
                logger().write("WARN", std::string("Replay timer failed: ") + ex.what());
                setStatus(*state, L"回放暂停：" + utf8ToWide(ex.what()));
            }
        }
        return 0;
    case WM_APP_STRATEGY_RESULT:
        if (state) {
            handleStrategyResult(*state);
        }
        return 0;
    case WM_COMMAND:
        if (!state) {
            break;
        }
        try {
            // 控件事件只做命令分发，业务逻辑集中在 market_controller/dialogs。
            switch (LOWORD(wParam)) {
            case IDC_SEARCH: searchSymbol(*state); break;
            case IDC_ADD: addFavorite(*state); break;
            case IDC_DELETE:
                if (state->detailView == DetailView::Holdings) {
                    deleteHolding(*state);
                } else if (state->detailView == DetailView::Strategy) {
                    deleteStrategySymbol(*state);
                } else {
                    deleteFavorite(*state);
                }
                break;
            case IDC_REFRESH:
                refreshDashboard(*state, true);
                if (state->detailView == DetailView::Strategy) {
                    startStrategyScan(*state);
                }
                break;
            case IDC_TECHNICAL_SWING_TOGGLE: toggleTechnicalSwing(*state); break;
            case IDC_SAVE_HOLDING: editHolding(*state); break;
            case IDC_TODAY_TRADES: editTodayTrades(*state); break;
            case IDC_ACCOUNT_FUNDS: editAccountFunds(*state); break;
            case IDC_DETAIL_TOGGLE:
                state->strategyDetailsShowTrades = !state->strategyDetailsShowTrades;
                configureSignalHistoryColumns(*state);
                updateToolbarVisibility(*state);
                populateDetailView(*state);
                break;
            case IDC_TOGGLE_STRATEGY:
                toggleStrategySymbol(*state);
                startStrategyScan(*state);
                break;
            case IDC_STRATEGY_COMBO:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    selectActiveStrategyFromCombo(*state);
                }
                break;
            case IDC_DATA_MODE_COMBO:
                if (HIWORD(wParam) == CBN_SELCHANGE) {
                    selectDataMode(*state);
                }
                break;
            case IDC_REPLAY_LOAD: loadReplaySession(*state); break;
            case IDC_REPLAY_PLAY: toggleReplayPlayback(*state); break;
            case IDC_REPLAY_STEP: advanceReplay(*state); break;
            case IDC_REPLAY_RESET: resetReplay(*state); break;
            case IDC_MANAGE_STRATEGIES:
                manageStrategies(*state);
                refreshResearchConfiguration(*state);
                break;
            case IDC_DAY_KLINE: switchKLine(*state, 101); break;
            case IDC_MIN_KLINE: switchKLine(*state, 1); break;
            case IDC_OPEN_LOGS: openLogDirectory(*state); break;
            case IDC_VIEW_FAVORITES: switchDetailView(*state, DetailView::Favorites); break;
            case IDC_VIEW_HOLDINGS: switchDetailView(*state, DetailView::Holdings); break;
            case IDC_VIEW_STRATEGY:
                switchDetailView(*state, DetailView::Strategy);
                startStrategyScan(*state);
                break;
            case IDC_VIEW_SCREENER:
                switchDetailView(*state, DetailView::Screener);
                break;
            case IDC_VIEW_RESEARCH:
                switchDetailView(*state, DetailView::Research);
                break;
            case IDC_VIEW_TRADE_ANALYSIS:
                switchDetailView(*state, DetailView::TradeAnalysis);
                break;
            }
        } catch (const std::exception& ex) {
            showError(*state, ex);
        }
        return 0;
    case WM_NOTIFY:
        if (state && reinterpret_cast<LPNMHDR>(lParam)->code == NM_CUSTOMDRAW) {
            HWND source = reinterpret_cast<LPNMHDR>(lParam)->hwndFrom;
            if (source == state->indexList || source == state->list ||
                source == state->signalHistoryList) {
                return drawListView(*state, reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            }
        }
        // 程序批量刷新列表时会产生 ITEMCHANGED，updatingLists 用于屏蔽这些伪用户操作。
        if (state && !state->updatingLists &&
            (reinterpret_cast<LPNMHDR>(lParam)->idFrom == IDC_LIST ||
             reinterpret_cast<LPNMHDR>(lParam)->idFrom == IDC_INDEX_LIST) &&
            reinterpret_cast<LPNMHDR>(lParam)->code == LVN_ITEMCHANGED) {
            auto* item = reinterpret_cast<LPNMLISTVIEW>(lParam);
            if ((item->uNewState & LVIS_SELECTED) != 0) {
                HWND sourceList = reinterpret_cast<LPNMHDR>(lParam)->hwndFrom;
                HWND otherList = sourceList == state->indexList ? state->list : state->indexList;
                ListView_SetItemState(otherList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                auto symbol = selectedSymbolFromList(sourceList);
                if (symbol) {
                    populateTechnicalSwingPanel(*state);
                    try {
                        SetWindowTextW(state->edit, utf8ToWide(*symbol).c_str());
                        if (sourceList == state->list &&
                            (state->detailView == DetailView::Holdings ||
                             state->detailView == DetailView::Strategy)) {
                            auto holding = state->holdings.find(*symbol);
                            if (holding != state->holdings.end()) {
                                long long quantity = holding->second.tradeBaseDate == currentDateToken()
                                                         ? holding->second.tradeBaseQuantity
                                                         : holding->second.quantity;
                                double cost = holding->second.tradeBaseDate == currentDateToken()
                                                  ? holding->second.tradeBaseCost
                                                  : holding->second.cost;
                                SetWindowTextW(state->quantityEdit, std::to_wstring(quantity).c_str());
                                SetWindowTextW(state->costEdit, formatNumber(cost, 3).c_str());
                            } else {
                                SetWindowTextW(state->quantityEdit, L"");
                                SetWindowTextW(state->costEdit, L"");
                            }
                        }
                        loadKLine(*state, *symbol);
                        updateHoldingSummary(*state);
                        setStatus(*state, L"已选择 " + utf8ToWide(*symbol));
                    } catch (const std::exception& ex) {
                        logger().write("WARN", std::string("Selection chart load failed: ") + ex.what());
                    }
                }
            }
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            KillTimer(hwnd, REFRESH_TIMER);
            KillTimer(hwnd, REPLAY_TIMER);
            // 先让后台线程停止投递窗口消息，再保存数据和释放窗口状态。
            state->strategyMailbox->alive.store(false);
            state->strategyMailbox->window.store(nullptr);
            try {
                state->store.save(state->favorites);
                state->holdingStore.save(state->holdings);
                state->holdingTradeStore.save(state->holdingTrades);
                state->strategyTradeStore.save(state->strategyTrades);
                state->accountFundsStore.save(state->accountFunds);
                state->strategyStore.save(state->strategySymbols);
                state->strategyConfigStore.save(state->strategies);
                state->strategyConfigStore.saveActive(state->activeStrategyId);
                if (state->dataMode == MarketDataMode::Live) {
                    const std::string date = currentDateToken();
                    if (state->strategySignalDate == date) {
                        state->strategySignalStore.save(date,
                                                        state->strategySignalHistory.records());
                    } else {
                        state->strategySignalStore.save(date, {});
                    }
                }
            } catch (const std::exception& ex) {
                logger().write("ERROR", std::string("Shutdown save failed: ") + ex.what());
            }
            logger().write("INFO", "Application stopped");
            DeleteObject(state->font);
            DeleteObject(state->titleFont);
            DeleteObject(state->sectionFont);
            DeleteObject(state->iconFont);
            DeleteObject(state->chartAxisFont);
            DeleteObject(state->chartValueFont);
            DeleteObject(state->windowBrush);
            DeleteObject(state->surfaceBrush);
            DeleteObject(state->inputBrush);
            DeleteObject(state->sidebarBrush);
            delete state;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace ashare
