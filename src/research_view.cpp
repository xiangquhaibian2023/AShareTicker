#include "research_view.h"

#include "chart_view.h"
#include "research_analysis.h"
#include "research_settings_dialog.h"
#include "resource_ids.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ashare {
namespace {

struct PendingResearchResult {
    int generation = 0;
    std::optional<AlgorithmResearchReport> report;
    std::string error;
};

// 后台线程只通过该邮箱向研究页投递进度和结果，不直接访问任何 HWND 控件。
struct ResearchMailbox {
    std::mutex mutex;
    std::wstring progress;
    PendingResearchResult pending;
    std::atomic_bool alive{true};
    std::atomic<HWND> window{nullptr};
    std::atomic_int generation{0};
};

struct ResearchLayout {
    RECT filter{};
    RECT summary{};
    RECT chart{};
    RECT listPanel{};
    RECT plot{};
};

enum class ResearchListMode {
    DailyResults,
    TradeSignals,
    Issues,
};

struct ResearchViewState {
    AppState* app = nullptr;
    HWND window = nullptr;
    HWND strategyCombo = nullptr;
    HWND scopeCombo = nullptr;
    HWND startDate = nullptr;
    HWND endDate = nullptr;
    HWND runButton = nullptr;
    HWND issuesButton = nullptr;
    HWND settingsButton = nullptr;
    HWND symbolFilter = nullptr;
    HWND dayFilter = nullptr;
    HWND status = nullptr;
    HWND list = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH inputBrush = nullptr;
    HFONT metricFont = nullptr;
    HFONT cardLabelFont = nullptr;
    HFONT smallFont = nullptr;
    std::shared_ptr<ResearchMailbox> mailbox = std::make_shared<ResearchMailbox>();
    std::optional<AlgorithmResearchReport> report;
    ResearchSimulationSettings simulationSettings;
    std::vector<std::string> symbolFilterOrder;
    ResearchLayout layout;
    bool running = false;
    bool showingIssues = false;
    int configuredListMode = -1;
};

ResearchViewState* researchState(HWND window) {
    return reinterpret_cast<ResearchViewState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring controlText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void setControlTextIfChanged(HWND control, const std::wstring& value) {
    if (controlText(control) != value) {
        SetWindowTextW(control, value.c_str());
    }
}

std::wstring dateDaysAgo(int days, bool skipWeekend) {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    FILETIME fileTime{};
    SystemTimeToFileTime(&value, &fileTime);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    do {
        ticks.QuadPart -= 24ULL * 60ULL * 60ULL * 10000000ULL;
        fileTime.dwLowDateTime = ticks.LowPart;
        fileTime.dwHighDateTime = ticks.HighPart;
        FileTimeToSystemTime(&fileTime, &value);
        --days;
    } while (days > 0 || (skipWeekend && (value.wDayOfWeek == 0 || value.wDayOfWeek == 6)));
    wchar_t buffer[16]{};
    std::swprintf(buffer, std::size(buffer), L"%04u-%02u-%02u",
                  value.wYear, value.wMonth, value.wDay);
    return buffer;
}

void addListColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.fmt = LVCFMT_CENTER;
    ListView_InsertColumn(list, index, &column);
}

void configureResearchListColumns(ResearchViewState& state, ResearchListMode mode) {
    int requestedMode = static_cast<int>(mode);
    if (state.configuredListMode == requestedMode &&
        Header_GetItemCount(ListView_GetHeader(state.list)) > 0) {
        return;
    }
    SendMessageW(state.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state.list);
    while (ListView_DeleteColumn(state.list, 0)) {
    }
    if (mode == ResearchListMode::Issues) {
        const wchar_t* columns[] = {L"日期 / 区间", L"代码", L"阶段", L"错误原因", L"处理建议"};
        const int widths[] = {150, 120, 110, 650, 280};
        for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
            addListColumn(state.list, index, columns[index], widths[index]);
        }
    } else if (mode == ResearchListMode::TradeSignals) {
        const wchar_t* columns[] = {
            L"时间", L"代码 / 名称", L"算法", L"方向", L"信号价格",
            L"信号数量", L"模拟成交价", L"成交数量", L"费用", L"状态", L"说明",
        };
        const int widths[] = {82, 190, 126, 58, 88, 88, 96, 88, 72, 76, 520};
        for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
            addListColumn(state.list, index, columns[index], widths[index]);
        }
    } else {
        const wchar_t* columns[] = {
            L"信号日", L"代码 / 名称", L"算法", L"方向", L"当日涨跌",
            L"后续观察日", L"基准价", L"对照价（最高 / 收盘）", L"后续收益",
            L"回测收益", L"T0 盈亏", L"费用", L"信号 / 成交",
        };
        const int widths[] = {96, 190, 126, 58, 82, 96, 74, 190, 82, 82, 82, 70, 90};
        for (int index = 0; index < static_cast<int>(std::size(columns)); ++index) {
            addListColumn(state.list, index, columns[index], widths[index]);
        }
    }
    state.configuredListMode = requestedMode;
    SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
}

std::wstring researchIssueSuggestion(const ResearchIssue& issue) {
    if (issue.stage == "分钟行情") {
        return L"程序会在下次分析时自动重试全部备用源";
    }
    if (issue.stage == "日线加载") {
        return L"检查网络连接后重新运行分析";
    }
    return L"查看运行日志并重新运行分析";
}

void updateIssuesButton(ResearchViewState& state) {
    size_t count = state.report ? state.report->issues.size() : 0;
    std::wstring text = state.showingIssues
                            ? L"返回研究明细"
                            : L"错误信息 (" + std::to_wstring(count) + L")";
    setControlTextIfChanged(state.issuesButton, text);
    EnableWindow(state.dayFilter, state.showingIssues ? FALSE : TRUE);
}

std::string selectedResearchResultSymbol(const ResearchViewState& state) {
    int selection = static_cast<int>(
        SendMessageW(state.symbolFilter, CB_GETCURSEL, 0, 0));
    if (selection <= 0 ||
        static_cast<size_t>(selection - 1) >= state.symbolFilterOrder.size()) {
        return {};
    }
    return state.symbolFilterOrder[static_cast<size_t>(selection - 1)];
}

const ResearchSymbolSummary* selectedResearchSymbolSummary(
    const ResearchViewState& state) {
    if (!state.report) return nullptr;
    std::string symbol = selectedResearchResultSymbol(state);
    if (symbol.empty()) return nullptr;
    auto found = state.report->symbolSummaries.find(symbol);
    return found == state.report->symbolSummaries.end() ? nullptr : &found->second;
}

const ResearchDayResult* selectedResearchDay(const ResearchViewState& state) {
    if (!state.report || state.showingIssues) return nullptr;
    int selection = static_cast<int>(
        SendMessageW(state.dayFilter, CB_GETCURSEL, 0, 0));
    if (selection <= 0 ||
        static_cast<size_t>(selection - 1) >= state.report->days.size()) {
        return nullptr;
    }
    return &state.report->days[static_cast<size_t>(selection - 1)];
}

const std::vector<ResearchCurvePoint>* activeResearchCurve(
    const ResearchViewState& state) {
    if (!state.report) return nullptr;
    if (const auto* summary = selectedResearchSymbolSummary(state)) {
        return &summary->curve;
    }
    return &state.report->curve;
}

ResearchLayout calculateResearchLayout(int width, int height) {
    ResearchLayout result;
    constexpr int margin = 10;
    constexpr int gap = 10;
    result.filter = RECT{margin, margin, width - margin, 150};
    result.summary = RECT{margin, result.filter.bottom + gap, width - margin,
                          result.filter.bottom + gap + 166};
    int remaining = height - result.summary.bottom - gap * 3;
    int chartHeight = std::clamp(static_cast<int>(remaining * 0.47), 170, 300);
    result.chart = RECT{margin, result.summary.bottom + gap, width - margin,
                        result.summary.bottom + gap + chartHeight};
    result.listPanel = RECT{margin, result.chart.bottom + gap, width - margin,
                            std::max<LONG>(result.chart.bottom + gap + 120,
                                           height - margin)};
    result.plot = RECT{result.chart.left + 66, result.chart.top + 66,
                       result.chart.right - 30, result.chart.bottom - 34};
    return result;
}

void drawRoundedPanel(HDC dc, const RECT& rect, COLORREF fill, COLORREF border,
                      int radius = 8) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void drawText(HDC dc, const std::wstring& value, RECT rect, HFONT font,
              COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HFONT oldFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    DrawTextW(dc, value.c_str(), -1, &rect, format | DT_NOPREFIX);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
}

std::wstring signedPercent(double value) {
    std::wostringstream output;
    output << std::showpos << std::fixed << std::setprecision(2) << value << L'%';
    return output.str();
}

COLORREF valueColor(double value, const ThemePalette& palette) {
    return value > 0.00001 ? palette.up : value < -0.00001 ? palette.down
                                                          : palette.text;
}

void drawMetricCard(HDC dc, const RECT& rect, const std::wstring& label,
                    const std::wstring& value, COLORREF valueColor,
                    const ResearchViewState& state, const ThemePalette& palette) {
    drawRoundedPanel(dc, rect, palette.surfaceAlt, palette.border);
    RECT labelRect{rect.left + 10, rect.top + 9, rect.right - 10, rect.top + 34};
    drawText(dc, label, labelRect, state.cardLabelFont, palette.muted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT valueRect{rect.left + 10, rect.top + 31, rect.right - 10, rect.bottom - 7};
    drawText(dc, value, valueRect, state.metricFont, valueColor,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void drawSummary(HDC dc, ResearchViewState& state, const ThemePalette& palette) {
    drawRoundedPanel(dc, state.layout.summary, palette.surface, palette.border);
    constexpr int gap = 10;
    int innerLeft = state.layout.summary.left + 12;
    int innerTop = state.layout.summary.top + 12;
    int innerWidth = state.layout.summary.right - state.layout.summary.left - 24;
    int cardWidth = (innerWidth - gap * 2) / 3;
    int cardHeight = (state.layout.summary.bottom - state.layout.summary.top - 34) / 2;

    const AlgorithmResearchReport* report = state.report ? &*state.report : nullptr;
    const ResearchSymbolSummary* symbol = selectedResearchSymbolSummary(state);
    std::wstring values[6];
    std::wstring labels[6];
    COLORREF colors[6]{};
    if (symbol) {
        values[0] = formatNumber(symbol->totalProfit, 2) + L" 元";
        values[1] = formatNumber(symbol->buyAndHoldProfit, 2) + L" 元";
        values[2] = formatNumber(symbol->strategyIncrementalProfit, 2) + L" 元";
        values[3] = signedPercent(symbol->totalReturnPercent);
        values[4] = formatNumber(symbol->t0Profit, 2) + L" 元";
        values[5] = std::to_wstring(symbol->tradeCount) + L" 笔 / " +
                    formatNumber(symbol->totalFees, 2) + L" 元";
        labels[0] = L"多日总盈亏";
        labels[1] = L"底仓持有盈亏";
        labels[2] = L"T0 增量盈亏";
        labels[3] = L"多日盈亏比例";
        labels[4] = L"T0 累计盈亏";
        labels[5] = L"成交 / 费用";
        colors[0] = valueColor(symbol->totalProfit, palette);
        colors[1] = valueColor(symbol->buyAndHoldProfit, palette);
        colors[2] = valueColor(symbol->strategyIncrementalProfit, palette);
        colors[3] = valueColor(symbol->totalReturnPercent, palette);
        colors[4] = valueColor(symbol->t0Profit, palette);
        colors[5] = palette.text;
    } else {
        values[0] = report ? formatNumber(report->simulationTotalProfit, 2) + L" 元" : L"--";
        values[1] = report ? formatNumber(report->buyAndHoldProfit, 2) + L" 元" : L"--";
        values[2] = report ? formatNumber(report->strategyIncrementalProfit, 2) + L" 元" : L"--";
        values[3] = report ? signedPercent(report->simulationCumulativePercent) : L"--";
        values[4] = report ? signedPercent(report->benchmarkCumulativePercent) : L"--";
        values[5] = report ? std::to_wstring(report->coveredTradingDays) + L" 个交易日" : L"--";
        labels[0] = L"全部标的总盈亏";
        labels[1] = L"底仓持有盈亏";
        labels[2] = L"T0 增量盈亏";
        labels[3] = L"自动回测收益率";
        labels[4] = L"上证指数同期";
        labels[5] = L"覆盖";
        colors[0] = report ? valueColor(report->simulationTotalProfit, palette) : palette.text;
        colors[1] = report ? valueColor(report->buyAndHoldProfit, palette) : palette.text;
        colors[2] = report ? valueColor(report->strategyIncrementalProfit, palette) : palette.text;
        colors[3] = report ? valueColor(report->simulationCumulativePercent, palette) : palette.text;
        colors[4] = report ? valueColor(report->benchmarkCumulativePercent, palette) : palette.text;
        colors[5] = palette.text;
    }
    for (int index = 0; index < 6; ++index) {
        int row = index / 3;
        int column = index % 3;
        RECT card{innerLeft + column * (cardWidth + gap),
                  innerTop + row * (cardHeight + gap),
                  innerLeft + column * (cardWidth + gap) + cardWidth,
                  innerTop + row * (cardHeight + gap) + cardHeight};
        drawMetricCard(dc, card, labels[index], values[index], colors[index],
                       state, palette);
    }
}

void drawLegendItem(HDC dc, int x, int y, COLORREF color,
                    const std::wstring& label, HFONT font,
                    const ThemePalette& palette) {
    HPEN pen = CreatePen(PS_SOLID, 3, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    MoveToEx(dc, x, y + 9, nullptr);
    LineTo(dc, x + 24, y + 9);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    drawText(dc, label, RECT{x + 32, y, x + 190, y + 20}, font, palette.muted,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void drawCurve(HDC dc, const std::vector<ResearchCurvePoint>& curve,
               const RECT& plot, double minimum, double maximum,
               double ResearchCurvePoint::*member, COLORREF color) {
    if (curve.size() < 2 || maximum <= minimum) {
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    for (size_t index = 0; index < curve.size(); ++index) {
        double value = curve[index].*member;
        int x = plot.left + static_cast<int>(std::lround(
                                static_cast<double>(plot.right - plot.left) * index /
                                static_cast<double>(curve.size() - 1)));
        int y = plot.bottom - static_cast<int>(std::lround(
                                 (value - minimum) / (maximum - minimum) *
                                 static_cast<double>(plot.bottom - plot.top)));
        if (index == 0) {
            MoveToEx(dc, x, y, nullptr);
        } else {
            LineTo(dc, x, y);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void drawResearchChart(HDC dc, ResearchViewState& state,
                       const ThemePalette& palette) {
    drawRoundedPanel(dc, state.layout.chart, palette.surface, palette.border);
    std::wstring heading = L"累计表现";
    if (state.report) {
        heading += L"  " + utf8ToWide(state.report->startDate) + L" 至 " +
                   utf8ToWide(state.report->endDate) + L"  ·  " +
                   utf8ToWide(state.report->strategy.name);
        if (const auto* summary = selectedResearchSymbolSummary(state)) {
            heading += L"  ·  " + utf8ToWide(summary->symbol) + L" " +
                       utf8ToWide(summary->name);
        }
    }
    drawText(dc, heading,
             RECT{state.layout.chart.left + 16, state.layout.chart.top + 10,
                  state.layout.chart.right - 16, state.layout.chart.top + 36},
             state.app->sectionFont, palette.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    constexpr COLORREF simulationColor = RGB(87, 139, 245);
    int legendY = state.layout.chart.top + 38;
    drawLegendItem(dc, state.layout.chart.left + 18, legendY, palette.accent,
                   L"算法信号", state.smallFont, palette);
    drawLegendItem(dc, state.layout.chart.left + 186, legendY, simulationColor,
                   L"自动回测", state.smallFont, palette);
    drawLegendItem(dc, state.layout.chart.left + 354, legendY, palette.muted,
                   L"上证指数", state.smallFont, palette);

    const auto* report = state.report ? &*state.report : nullptr;
    const auto* curve = activeResearchCurve(state);
    if (!report || !curve || curve->empty()) {
        drawText(dc, L"选择算法和真实历史区间后开始分析",
                 state.layout.plot, state.app->font, palette.muted,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    double minimum = 0.0;
    double maximum = 0.0;
    for (const auto& point : *curve) {
        minimum = std::min({minimum, point.signalCumulativePercent,
                            point.simulationCumulativePercent,
                            point.benchmarkCumulativePercent});
        maximum = std::max({maximum, point.signalCumulativePercent,
                            point.simulationCumulativePercent,
                            point.benchmarkCumulativePercent});
    }
    double span = std::max(2.0, maximum - minimum);
    minimum -= span * 0.12;
    maximum += span * 0.12;

    HPEN gridPen = CreatePen(PS_SOLID, 1, palette.grid);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, gridPen));
    for (int index = 0; index <= 4; ++index) {
        int y = state.layout.plot.top +
                (state.layout.plot.bottom - state.layout.plot.top) * index / 4;
        MoveToEx(dc, state.layout.plot.left, y, nullptr);
        LineTo(dc, state.layout.plot.right, y);
        double value = maximum - (maximum - minimum) * index / 4.0;
        drawText(dc, signedPercent(value),
                 RECT{state.layout.chart.left + 8, y - 10,
                      state.layout.plot.left - 8, y + 10},
                 state.smallFont, palette.muted,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);

    drawCurve(dc, *curve, state.layout.plot, minimum, maximum,
              &ResearchCurvePoint::signalCumulativePercent, palette.accent);
    drawCurve(dc, *curve, state.layout.plot, minimum, maximum,
              &ResearchCurvePoint::simulationCumulativePercent, simulationColor);
    drawCurve(dc, *curve, state.layout.plot, minimum, maximum,
              &ResearchCurvePoint::benchmarkCumulativePercent, palette.muted);

    size_t middle = curve->size() / 2;
    drawText(dc, utf8ToWide(curve->front().date.substr(5)),
             RECT{state.layout.plot.left - 25, state.layout.plot.bottom + 4,
                  state.layout.plot.left + 55, state.layout.plot.bottom + 24},
             state.smallFont, palette.muted,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, utf8ToWide((*curve)[middle].date.substr(5)),
             RECT{(state.layout.plot.left + state.layout.plot.right) / 2 - 45,
                  state.layout.plot.bottom + 4,
                  (state.layout.plot.left + state.layout.plot.right) / 2 + 45,
                  state.layout.plot.bottom + 24},
             state.smallFont, palette.muted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, utf8ToWide(curve->back().date.substr(5)),
             RECT{state.layout.plot.right - 55, state.layout.plot.bottom + 4,
                  state.layout.plot.right + 25, state.layout.plot.bottom + 24},
             state.smallFont, palette.muted,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    int selection = static_cast<int>(SendMessageW(state.dayFilter, CB_GETCURSEL, 0, 0));
    if (selection > 0 && static_cast<size_t>(selection - 1) < curve->size()) {
        size_t index = static_cast<size_t>(selection - 1);
        int x = curve->size() == 1
                    ? (state.layout.plot.left + state.layout.plot.right) / 2
                    : state.layout.plot.left + static_cast<int>(
                          (state.layout.plot.right - state.layout.plot.left) * index /
                          (curve->size() - 1));
        HPEN selectedPen = CreatePen(PS_DOT, 1, palette.accent);
        oldPen = static_cast<HPEN>(SelectObject(dc, selectedPen));
        MoveToEx(dc, x, state.layout.plot.top, nullptr);
        LineTo(dc, x, state.layout.plot.bottom);
        SelectObject(dc, oldPen);
        DeleteObject(selectedPen);
    }
}

void insertListText(HWND list, int row, int column, const std::wstring& value) {
    if (column == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<LPWSTR>(value.c_str());
        ListView_InsertItem(list, &item);
    } else {
        ListView_SetItemText(list, row, column,
                             const_cast<LPWSTR>(value.c_str()));
    }
}

void populateResearchList(ResearchViewState& state) {
    const ResearchDayResult* selectedDay = selectedResearchDay(state);
    ResearchListMode mode = state.showingIssues
                                ? ResearchListMode::Issues
                                : selectedDay ? ResearchListMode::TradeSignals
                                              : ResearchListMode::DailyResults;
    configureResearchListColumns(state, mode);
    SendMessageW(state.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state.list);
    if (!state.report) {
        SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(state.list, nullptr, TRUE);
        return;
    }
    if (state.showingIssues) {
        std::string selectedSymbol = selectedResearchResultSymbol(state);
        int row = 0;
        for (const auto& issue : state.report->issues) {
            if (!selectedSymbol.empty() && issue.symbol != selectedSymbol) {
                continue;
            }
            std::wstring values[] = {
                utf8ToWide(issue.date), utf8ToWide(issue.symbol),
                utf8ToWide(issue.stage), utf8ToWide(issue.message),
                researchIssueSuggestion(issue),
            };
            for (int column = 0; column < static_cast<int>(std::size(values)); ++column) {
                insertListText(state.list, row, column, values[column]);
            }
            ++row;
        }
        SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(state.list, nullptr, TRUE);
        return;
    }
    std::string selectedSymbol = selectedResearchResultSymbol(state);
    int row = 0;
    if (selectedDay) {
        for (const auto& detail : selectedDay->tradeSignals) {
            if (!selectedSymbol.empty() && detail.symbol != selectedSymbol) {
                continue;
            }
            int precision = isFundLikeSymbol(detail.symbol) ? 3 : 2;
            std::wstring symbolName = utf8ToWide(detail.symbol);
            if (!detail.name.empty()) {
                symbolName += L"  " + utf8ToWide(detail.name);
            }
            std::wstring time = utf8ToWide(detail.time);
            size_t separator = time.find(L' ');
            if (separator != std::wstring::npos && separator + 1 < time.size()) {
                time = time.substr(separator + 1);
            }
            std::wstring values[] = {
                time, symbolName, utf8ToWide(detail.algorithm),
                detail.direction == SignalType::Buy ? L"买入" : L"卖出",
                formatNumber(detail.signalPrice, precision),
                detail.signalQuantity > 0 ? std::to_wstring(detail.signalQuantity) : L"--",
                detail.executed ? formatNumber(detail.executionPrice, precision) : L"--",
                detail.executed ? std::to_wstring(detail.executionQuantity) : L"--",
                detail.executed ? formatNumber(detail.fees, 2) : L"--",
                detail.executed ? L"已成交" : L"未成交",
                utf8ToWide(detail.description),
            };
            for (int column = 0; column < static_cast<int>(std::size(values)); ++column) {
                insertListText(state.list, row, column, values[column]);
            }
            ++row;
        }
        SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(state.list, nullptr, TRUE);
        return;
    }
    for (size_t dayIndex = 0; dayIndex < state.report->days.size(); ++dayIndex) {
        for (const auto& item : state.report->days[dayIndex].securities) {
            if (!selectedSymbol.empty() && item.symbol != selectedSymbol) {
                continue;
            }
            int precision = isFundLikeSymbol(item.symbol) ? 3 : 2;
            std::wstring symbolName = utf8ToWide(item.symbol);
            if (!item.name.empty()) {
                symbolName += L"  " + utf8ToWide(item.name);
            }
            std::wstring signalTrade = std::to_wstring(item.signalCount) + L" / " +
                                       std::to_wstring(item.tradeCount);
            std::wstring values[] = {
                utf8ToWide(item.signalDate), symbolName, utf8ToWide(item.algorithm),
                item.direction == SignalType::Buy ? L"买入" : L"卖出",
                signedPercent(item.signalDayChangePercent),
                utf8ToWide(item.followupDate), formatNumber(item.basePrice, precision),
                formatNumber(item.comparisonPrice, precision) + L"  (" +
                    formatNumber(item.followupHigh, precision) + L" / " +
                    formatNumber(item.followupClose, precision) + L")",
                signedPercent(item.followupReturnPercent),
                signedPercent(item.simulationReturnPercent),
                formatNumber(item.t0Profit, 2), formatNumber(item.fees, 2), signalTrade,
            };
            for (int column = 0; column < static_cast<int>(std::size(values)); ++column) {
                insertListText(state.list, row, column, values[column]);
            }
            ++row;
        }
    }
    SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.list, nullptr, TRUE);
}

void populateDayFilter(ResearchViewState& state) {
    SendMessageW(state.dayFilter, CB_RESETCONTENT, 0, 0);
    SendMessageW(state.dayFilter, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"全部交易日"));
    if (state.report) {
        for (const auto& day : state.report->days) {
            std::wstring value = utf8ToWide(day.signalDate);
            if (!day.tradeSignals.empty()) {
                value += L"  ·  " + std::to_wstring(day.tradeSignals.size()) + L" 条信号";
            }
            SendMessageW(state.dayFilter, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(value.c_str()));
        }
    }
    SendMessageW(state.dayFilter, CB_SETCURSEL, 0, 0);
}

void layoutResearchControls(ResearchViewState& state, int width, int height) {
    state.layout = calculateResearchLayout(width, height);
    int left = state.layout.filter.left + 16;
    int right = state.layout.filter.right - 16;
    constexpr int controlHeight = 34;
    constexpr int labelWidth = 56;
    constexpr int strategyWidth = 170;
    constexpr int scopeLabelWidth = 72;
    constexpr int scopeWidth = 140;
    constexpr int symbolLabelWidth = 64;
    constexpr int symbolWidth = 170;
    constexpr int gap = 12;
    int rowOne = state.layout.filter.top + 16;
    int rowTwo = rowOne + 44;

    MoveWindow(state.strategyCombo, left + labelWidth, rowOne, strategyWidth, 220, TRUE);
    int scopeLabelX = left + labelWidth + strategyWidth + 14;
    MoveWindow(state.scopeCombo, scopeLabelX + scopeLabelWidth, rowOne,
               scopeWidth, 180, TRUE);
    int symbolLabelX = scopeLabelX + scopeLabelWidth + scopeWidth + 14;
    MoveWindow(state.symbolFilter, symbolLabelX + symbolLabelWidth, rowOne,
               symbolWidth, 220, TRUE);
    MoveWindow(state.issuesButton, right - 124, rowOne, 124, controlHeight, TRUE);
    int dateX = left + labelWidth;
    MoveWindow(state.startDate, dateX, rowTwo, 112, controlHeight, TRUE);
    int endLabelX = dateX + 112 + gap;
    MoveWindow(state.endDate, endLabelX + 44, rowTwo, 112, controlHeight, TRUE);
    int settingsX = endLabelX + 44 + 112 + gap;
    MoveWindow(state.settingsButton, settingsX, rowTwo, 112, controlHeight, TRUE);
    int runX = settingsX + 112 + gap;
    MoveWindow(state.runButton, runX, rowTwo, 112, controlHeight, TRUE);
    int dayWidth = std::min(190, std::max(130, right - runX - 112 - 88));
    MoveWindow(state.dayFilter, right - dayWidth, rowTwo, dayWidth, 220, TRUE);
    MoveWindow(state.status, left, state.layout.filter.bottom - 25,
               std::max(120, right - left), 18, TRUE);

    MoveWindow(state.list, state.layout.listPanel.left + 10,
               state.layout.listPanel.top + 38,
               state.layout.listPanel.right - state.layout.listPanel.left - 20,
               std::max<LONG>(50, state.layout.listPanel.bottom -
                                      state.layout.listPanel.top - 48), TRUE);
}

void paintResearchView(HWND window, ResearchViewState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HDC memory = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memory, bitmap));
    ThemePalette palette = themePalette();
    FillRect(memory, &client, state.windowBrush);

    drawRoundedPanel(memory, state.layout.filter, palette.surface, palette.border);
    drawText(memory, L"算法", RECT{state.layout.filter.left + 16,
                                    state.layout.filter.top + 16,
                                    state.layout.filter.left + 72,
                                    state.layout.filter.top + 50},
             state.app->font, palette.text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    int scopeLabelX = state.layout.filter.left + 16 + 56 + 170 + 14;
    drawText(memory, L"标的范围", RECT{scopeLabelX, state.layout.filter.top + 16,
                                        scopeLabelX + 72, state.layout.filter.top + 50},
             state.app->font, palette.text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    int symbolLabelX = scopeLabelX + 72 + 140 + 14;
    drawText(memory, L"查看标的", RECT{symbolLabelX, state.layout.filter.top + 16,
                                        symbolLabelX + 64, state.layout.filter.top + 50},
             state.app->font, palette.text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    drawText(memory, L"开始", RECT{state.layout.filter.left + 16,
                                    state.layout.filter.top + 60,
                                    state.layout.filter.left + 72,
                                    state.layout.filter.top + 94},
             state.app->font, palette.text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    int endLabelX = state.layout.filter.left + 16 + 56 + 112 + 12;
    drawText(memory, L"结束", RECT{endLabelX, state.layout.filter.top + 60,
                                    endLabelX + 44, state.layout.filter.top + 94},
             state.app->font, palette.text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    int right = state.layout.filter.right - 16;
    int dayWidth = static_cast<int>(std::min<LONG>(190, std::max<LONG>(130, right -
        (state.layout.filter.left + 16 + 56 + 112 + 12 + 44 + 112 + 12 + 112 + 12 + 112) - 88)));
    RECT dayLabel{right - dayWidth - 74, state.layout.filter.top + 60,
                  right - dayWidth - 8, state.layout.filter.top + 94};
    drawText(memory, L"明细日期", dayLabel, state.app->font, palette.text,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    drawSummary(memory, state, palette);
    drawResearchChart(memory, state, palette);
    drawRoundedPanel(memory, state.layout.listPanel, palette.surface, palette.border);
    std::wstring listHeading = state.showingIssues ? L"错误信息" : L"信号日研究明细";
    if (state.report && state.showingIssues) {
        listHeading += L"  ·  " + std::to_wstring(state.report->issues.size()) + L" 项";
    } else if (const auto* day = selectedResearchDay(state)) {
        std::string selectedSymbol = selectedResearchResultSymbol(state);
        int buyCount = 0;
        int sellCount = 0;
        int executedCount = 0;
        for (const auto& detail : day->tradeSignals) {
            if (!selectedSymbol.empty() && detail.symbol != selectedSymbol) continue;
            if (detail.direction == SignalType::Buy) ++buyCount;
            if (detail.direction == SignalType::Sell) ++sellCount;
            if (detail.executed) ++executedCount;
        }
        listHeading = utf8ToWide(day->signalDate) + L"  交易信号明细";
        if (!selectedSymbol.empty()) {
            listHeading += L"  ·  " + utf8ToWide(selectedSymbol);
        }
        listHeading += L"  ·  买入 " + std::to_wstring(buyCount) +
                       L" 条  ·  卖出 " + std::to_wstring(sellCount) +
                       L" 条  ·  成交 " + std::to_wstring(executedCount) + L" 笔";
    } else if (const auto* summary = selectedResearchSymbolSummary(state)) {
        listHeading = utf8ToWide(summary->symbol) + L" " + utf8ToWide(summary->name) +
                      L"  ·  多日总盈亏 " + formatNumber(summary->totalProfit, 2) +
                      L" 元  ·  盈亏比例 " + signedPercent(summary->totalReturnPercent) +
                      L"  ·  期末持仓 " + std::to_wstring(summary->endingQuantity);
    } else if (state.report) {
        listHeading += L"  ·  " + std::to_wstring(state.report->securityResultCount) +
                       L" 条  ·  " + std::to_wstring(state.report->signalDays) + L" 个信号日";
    }
    drawText(memory, listHeading,
             RECT{state.layout.listPanel.left + 14, state.layout.listPanel.top + 7,
                  state.layout.listPanel.right - 14, state.layout.listPanel.top + 34},
             state.app->font, palette.text,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    BitBlt(target, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

std::map<std::string, std::string> selectedResearchSymbols(
    const ResearchViewState& state) {
    std::map<std::string, std::string> result;
    int scope = static_cast<int>(SendMessageW(state.scopeCombo, CB_GETCURSEL, 0, 0));
    auto add = [&](const std::string& symbol) {
        std::string name;
        auto known = state.app->symbolNames.find(symbol);
        if (known != state.app->symbolNames.end()) {
            name = known->second;
        } else {
            auto quote = state.app->quoteCache.find(symbol);
            if (quote != state.app->quoteCache.end()) {
                name = quote->second.name;
            }
        }
        result[symbol] = name.empty() ? symbol : name;
    };
    if (scope == 1) {
        for (const auto& [symbol, holding] : state.app->holdings) {
            if (holding.quantity > 0) add(symbol);
        }
    } else if (scope == 2) {
        for (const auto& symbol : state.app->favorites) add(symbol);
    } else {
        for (const auto& symbol : state.app->strategySymbols) add(symbol);
    }
    return result;
}

void populateResearchSymbolFilter(ResearchViewState& state,
                                  const std::string& preferred = {}) {
    std::string current = preferred.empty() ? selectedResearchResultSymbol(state)
                                            : preferred;
    auto symbols = selectedResearchSymbols(state);
    SendMessageW(state.symbolFilter, CB_RESETCONTENT, 0, 0);
    SendMessageW(state.symbolFilter, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"全部标的"));
    state.symbolFilterOrder.clear();
    int selected = 0;
    for (const auto& [symbol, name] : symbols) {
        state.symbolFilterOrder.push_back(symbol);
        std::wstring label = utf8ToWide(symbol);
        if (!name.empty() && name != symbol) label += L"  " + utf8ToWide(name);
        SendMessageW(state.symbolFilter, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
        if (symbol == current) {
            selected = static_cast<int>(state.symbolFilterOrder.size());
        }
        if (state.simulationSettings.openingHoldings.find(symbol) ==
            state.simulationSettings.openingHoldings.end()) {
            auto appHolding = state.app->holdings.find(symbol);
            if (appHolding != state.app->holdings.end()) {
                state.simulationSettings.openingHoldings[symbol] = appHolding->second;
            } else {
                Holding holding;
                holding.symbol = symbol;
                state.simulationSettings.openingHoldings.emplace(symbol, holding);
            }
        }
        state.simulationSettings.feesBySymbol.emplace(
            symbol, state.simulationSettings.fees);
    }
    SendMessageW(state.symbolFilter, CB_SETCURSEL, selected, 0);
}

void startResearch(ResearchViewState& state) {
    if (state.running) {
        return;
    }
    // 主窗口创建研究页时持仓和自选可能尚未完成加载，运行前再次同步当前范围。
    populateResearchSymbolFilter(state);
    int selection = static_cast<int>(
        SendMessageW(state.strategyCombo, CB_GETCURSEL, 0, 0));
    if (selection < 0 || selection >= static_cast<int>(state.app->strategies.size())) {
        MessageBoxW(state.window, L"请先选择需要分析的算法。", L"算法研究",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    AlgorithmResearchRequest request;
    request.strategy = state.app->strategies[static_cast<size_t>(selection)];
    request.symbols = selectedResearchSymbols(state);
    for (const auto& [symbol, name] : request.symbols) {
        (void)name;
        auto holding = state.simulationSettings.openingHoldings.find(symbol);
        if (holding != state.simulationSettings.openingHoldings.end()) {
            request.openingHoldings.emplace(symbol, holding->second);
        }
        auto fees = state.simulationSettings.feesBySymbol.find(symbol);
        if (fees != state.simulationSettings.feesBySymbol.end()) {
            request.feesBySymbol.emplace(symbol, fees->second);
        }
    }
    request.initialCash = state.simulationSettings.initialCash;
    request.fees = state.simulationSettings.fees;
    request.startDate = wideToUtf8(controlText(state.startDate));
    request.endDate = wideToUtf8(controlText(state.endDate));
    request.replayCacheDirectory = applicationStorageDirectory() / "replay_cache";
    if (request.symbols.empty()) {
        MessageBoxW(state.window, L"所选范围没有标的，请先添加自选、持仓或策略观察标的。",
                    L"算法研究", MB_OK | MB_ICONINFORMATION);
        return;
    }

    state.running = true;
    state.showingIssues = false;
    updateIssuesButton(state);
    EnableWindow(state.runButton, FALSE);
    SetWindowTextW(state.runButton, L"分析中");
    setControlTextIfChanged(state.status, L"正在准备真实历史行情...");
    int generation = state.mailbox->generation.fetch_add(1) + 1;
    auto mailbox = state.mailbox;
    logger().write("INFO", "Research started strategy=" + request.strategy.id +
                               " symbols=" + std::to_string(request.symbols.size()) +
                               " range=" + request.startDate + ".." + request.endDate +
                               " cash=" + std::to_string(request.initialCash) +
                               " feeTemplates=" +
                               std::to_string(request.feesBySymbol.size()));

    std::thread([request = std::move(request), mailbox, generation]() mutable {
        PendingResearchResult pending;
        pending.generation = generation;
        try {
            auto progress = [mailbox, generation](int completed, int total,
                                                  const std::string& message) {
                if (!mailbox->alive.load() ||
                    mailbox->generation.load() != generation) {
                    return;
                }
                int percentage = total > 0 ? completed * 100 / total : 0;
                std::wstring text = utf8ToWide(message) + L"  " +
                                    std::to_wstring(percentage) + L"%";
                {
                    std::lock_guard<std::mutex> lock(mailbox->mutex);
                    mailbox->progress = std::move(text);
                }
                HWND window = mailbox->window.load();
                if (window) PostMessageW(window, WM_APP_RESEARCH_PROGRESS, 0, 0);
            };
            auto cancelled = [mailbox, generation]() {
                return !mailbox->alive.load() ||
                       mailbox->generation.load() != generation;
            };
            pending.report = runAlgorithmResearch(request, progress, cancelled);
        } catch (const std::exception& error) {
            pending.error = error.what();
        }
        if (!mailbox->alive.load() || mailbox->generation.load() != generation) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->pending = std::move(pending);
        }
        HWND window = mailbox->window.load();
        if (window) PostMessageW(window, WM_APP_RESEARCH_RESULT, 0, 0);
    }).detach();
}

void handleResearchResult(ResearchViewState& state) {
    PendingResearchResult pending;
    {
        std::lock_guard<std::mutex> lock(state.mailbox->mutex);
        pending = std::move(state.mailbox->pending);
    }
    if (pending.generation != state.mailbox->generation.load()) {
        return;
    }
    state.running = false;
    EnableWindow(state.runButton, TRUE);
    SetWindowTextW(state.runButton, L"开始分析");
    if (!pending.error.empty()) {
        setControlTextIfChanged(state.status, L"分析失败：" + utf8ToWide(pending.error));
        logger().write("WARN", "Research failed: " + pending.error);
        MessageBoxW(state.window, utf8ToWide(pending.error).c_str(), L"算法研究失败",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    state.report = std::move(pending.report);
    state.showingIssues = false;
    populateDayFilter(state);
    populateResearchList(state);
    updateIssuesButton(state);
    if (state.report) {
        std::wstring status = L"分析完成：覆盖 " +
                              std::to_wstring(state.report->coveredTradingDays) +
                              L" 个交易日，产生 " +
                              std::to_wstring(state.report->securityResultCount) + L" 条结果";
        if (!state.report->issues.empty()) {
            status += L"，失败 " + std::to_wstring(state.report->issues.size()) +
                      L" 项，可查看错误信息";
        }
        setControlTextIfChanged(state.status, status);
        logger().write("INFO", "Research completed results=" +
                                   std::to_string(state.report->securityResultCount) +
                                   " issues=" +
                                   std::to_string(state.report->issues.size()));
        for (const auto& issue : state.report->issues) {
            logger().write("WARN", "Research issue date=" + issue.date +
                                       " symbol=" + issue.symbol +
                                       " stage=" + issue.stage +
                                       " error=" + issue.message);
        }
    }
    InvalidateRect(state.window, nullptr, FALSE);
}

LRESULT drawResearchList(NMLVCUSTOMDRAW* draw) {
    ThemePalette palette = themePalette();
    ResearchViewState* state = researchState(GetParent(draw->nmcd.hdr.hwndFrom));
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int row = static_cast<int>(draw->nmcd.dwItemSpec);
        bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
        draw->clrTextBk = selected ? palette.selection
                                   : row % 2 == 0 ? palette.surfaceAlt : palette.surface;
        draw->clrText = palette.text;
        int column = draw->iSubItem;
        if (state && state->showingIssues) {
            if (column == 2 || column == 3) {
                draw->clrText = palette.down;
            }
        } else if (state && state->configuredListMode ==
                                static_cast<int>(ResearchListMode::TradeSignals)) {
            if (column == 3) {
                wchar_t text[16]{};
                ListView_GetItemText(draw->nmcd.hdr.hwndFrom, row, column, text, 16);
                draw->clrText = std::wstring(text) == L"买入" ? palette.up : palette.down;
            } else if (column == 9) {
                wchar_t text[16]{};
                ListView_GetItemText(draw->nmcd.hdr.hwndFrom, row, column, text, 16);
                draw->clrText = std::wstring(text) == L"已成交" ? palette.accent
                                                                : palette.muted;
            }
        } else if (column == 3) {
            wchar_t text[16]{};
            ListView_GetItemText(draw->nmcd.hdr.hwndFrom, row, column, text, 16);
            draw->clrText = std::wstring(text) == L"买入" ? palette.up : palette.down;
        } else if (column == 4 || column == 8 || column == 9 || column == 10) {
            wchar_t text[64]{};
            ListView_GetItemText(draw->nmcd.hdr.hwndFrom, row, column, text, 64);
            double value = std::wcstod(text, nullptr);
            draw->clrText = valueColor(value, palette);
        }
        return CDRF_NEWFONT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

}  // namespace

void refreshResearchConfiguration(AppState& app) {
    if (!app.researchView) {
        return;
    }
    ResearchViewState* state = researchState(app.researchView);
    if (!state || !state->strategyCombo) {
        return;
    }
    std::string selectedId = app.activeStrategyId;
    SendMessageW(state->strategyCombo, CB_RESETCONTENT, 0, 0);
    int selected = 0;
    for (int index = 0; index < static_cast<int>(app.strategies.size()); ++index) {
        const auto& strategy = app.strategies[static_cast<size_t>(index)];
        std::wstring label = utf8ToWide(strategy.name);
        SendMessageW(state->strategyCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
        if (strategy.id == selectedId) selected = index;
    }
    if (!app.strategies.empty()) {
        SendMessageW(state->strategyCombo, CB_SETCURSEL, selected, 0);
    }
    populateResearchSymbolFilter(*state);
}

LRESULT CALLBACK ResearchViewProc(HWND window, UINT message, WPARAM wParam,
                                  LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* app = reinterpret_cast<AppState*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        auto* state = new ResearchViewState();
        state->app = app;
        state->window = window;
        if (app->accountFunds.availableCash > 0.0) {
            state->simulationSettings.initialCash = app->accountFunds.availableCash;
        }
        state->mailbox->window.store(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        ThemePalette palette = themePalette();
        state->windowBrush = CreateSolidBrush(palette.window);
        state->surfaceBrush = CreateSolidBrush(palette.surface);
        state->inputBrush = CreateSolidBrush(palette.surfaceAlt);
        state->metricFont = CreateFontW(-23, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        state->cardLabelFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                           DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
        state->smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                       DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");

        auto combo = [&](int id) {
            HWND control = CreateWindowExW(
                0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                    CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(id), app->instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            installRoundedComboTheme(control);
            return control;
        };
        auto edit = [&](int id, const std::wstring& value) {
            HWND control = CreateWindowExW(
                0, L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                    ES_AUTOHSCROLL | ES_CENTER | ES_MULTILINE,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(id), app->instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELPARAM(8, 8));
            SendMessageW(control, EM_SETLIMITTEXT, 10, 0);
            installRoundedInputTheme(control);
            return control;
        };
        state->strategyCombo = combo(IDC_RESEARCH_STRATEGY);
        state->scopeCombo = combo(IDC_RESEARCH_SCOPE);
        state->symbolFilter = combo(IDC_RESEARCH_SYMBOL_FILTER);
        state->startDate = edit(IDC_RESEARCH_START_DATE, dateDaysAgo(21, false));
        state->endDate = edit(IDC_RESEARCH_END_DATE, dateDaysAgo(1, true));
        state->dayFilter = combo(IDC_RESEARCH_DAY_FILTER);
        state->runButton = CreateWindowW(
            L"BUTTON", L"开始分析", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_RESEARCH_RUN),
            app->instance, nullptr);
        SendMessageW(state->runButton, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        state->settingsButton = CreateWindowW(
            L"BUTTON", L"回测参数",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_RESEARCH_SETTINGS),
            app->instance, nullptr);
        SendMessageW(state->settingsButton, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        state->issuesButton = CreateWindowW(
            L"BUTTON", L"错误信息 (0)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_RESEARCH_ISSUES),
            app->instance, nullptr);
        SendMessageW(state->issuesButton, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        state->status = CreateWindowW(
            L"STATIC", L"选择算法、标的范围和日期区间后开始批量分析。",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(IDC_RESEARCH_STATUS), app->instance, nullptr);
        SendMessageW(state->status, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state->smallFont), TRUE);
        state->list = CreateWindowExW(
            0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_RESEARCH_LIST),
            app->instance, nullptr);
        SendMessageW(state->list, WM_SETFONT,
                     reinterpret_cast<WPARAM>(app->font), TRUE);
        ListView_SetExtendedListViewStyle(
            state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(state->list, palette.surface);
        ListView_SetTextBkColor(state->list, palette.surface);
        ListView_SetTextColor(state->list, palette.text);
        setExplorerControlTheme(state->list);
        setExplorerControlTheme(ListView_GetHeader(state->list));
        SetWindowSubclass(ListView_GetHeader(state->list), HeaderProc, 4,
                          reinterpret_cast<DWORD_PTR>(app));
        configureResearchListColumns(*state, ResearchListMode::DailyResults);
        SendMessageW(state->scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"策略观察标的"));
        SendMessageW(state->scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"当前持仓标的"));
        SendMessageW(state->scopeCombo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"全部自选标的"));
        SendMessageW(state->scopeCombo, CB_SETCURSEL, 0, 0);
        populateResearchSymbolFilter(*state);
        populateDayFilter(*state);
        updateIssuesButton(*state);
        refreshResearchConfiguration(*app);
        RECT client{};
        GetClientRect(window, &client);
        layoutResearchControls(*state, client.right, client.bottom);
        return 0;
    }

    ResearchViewState* state = researchState(window);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (state) {
            layoutResearchControls(*state, LOWORD(lParam), HIWORD(lParam));
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_PAINT:
        if (state) {
            paintResearchView(window, *state);
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
    case WM_CTLCOLOREDIT:
        if (state) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, themePalette().surfaceAlt);
            SetTextColor(dc, themePalette().text);
            return reinterpret_cast<LRESULT>(state->inputBrush);
        }
        break;
    case WM_DRAWITEM:
        if (state) {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                drawModalButton(*item, item->CtlID == IDC_RESEARCH_RUN);
                return TRUE;
            }
            if (item->CtlType == ODT_COMBOBOX) {
                return drawThemedComboItem(*item);
            }
        }
        break;
    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wParam) == IDC_RESEARCH_RUN && HIWORD(wParam) == BN_CLICKED) {
            startResearch(*state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_SETTINGS && HIWORD(wParam) == BN_CLICKED) {
            try {
                if (editResearchSimulationSettings(
                        state->app->window, state->app->instance, state->app->font,
                        selectedResearchSymbols(*state), state->simulationSettings)) {
                    std::wstring message = L"回测参数已保存：期初资金 " +
                                           formatNumber(state->simulationSettings.initialCash, 2) +
                                           L" 元，已配置 " +
                                           std::to_wstring(selectedResearchSymbols(*state).size()) +
                                           L" 个标的的独立费率";
                    setControlTextIfChanged(state->status, message);
                }
            } catch (const std::exception& error) {
                MessageBoxW(state->window, utf8ToWide(error.what()).c_str(),
                            L"回测参数", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_ISSUES && HIWORD(wParam) == BN_CLICKED) {
            state->showingIssues = !state->showingIssues;
            updateIssuesButton(*state);
            populateResearchList(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_DAY_FILTER &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            populateResearchList(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_SCOPE &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            populateResearchSymbolFilter(*state);
            populateResearchList(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_SYMBOL_FILTER &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            populateResearchList(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (state && reinterpret_cast<LPNMHDR>(lParam)->hwndFrom == state->list &&
            reinterpret_cast<LPNMHDR>(lParam)->code == NM_CUSTOMDRAW) {
            return drawResearchList(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
        }
        break;
    case WM_LBUTTONUP:
        if (state && state->report && !state->report->curve.empty()) {
            POINT point{static_cast<short>(LOWORD(lParam)),
                        static_cast<short>(HIWORD(lParam))};
            if (PtInRect(&state->layout.plot, point)) {
                int width = std::max<LONG>(1, state->layout.plot.right -
                                                  state->layout.plot.left);
                double ratio = std::clamp(
                    static_cast<double>(point.x - state->layout.plot.left) / width,
                    0.0, 1.0);
                size_t index = static_cast<size_t>(std::lround(
                    ratio * static_cast<double>(state->report->curve.size() - 1)));
                SendMessageW(state->dayFilter, CB_SETCURSEL,
                             static_cast<WPARAM>(index + 1), 0);
                populateResearchList(*state);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
        }
        break;
    case WM_APP_RESEARCH_PROGRESS:
        if (state) {
            std::wstring progress;
            {
                std::lock_guard<std::mutex> lock(state->mailbox->mutex);
                progress = state->mailbox->progress;
            }
            setControlTextIfChanged(state->status, progress);
        }
        return 0;
    case WM_APP_RESEARCH_RESULT:
        if (state) handleResearchResult(*state);
        return 0;
    case WM_NCDESTROY:
        if (state) {
            state->mailbox->alive.store(false);
            state->mailbox->window.store(nullptr);
            state->mailbox->generation.fetch_add(1);
            DeleteObject(state->windowBrush);
            DeleteObject(state->surfaceBrush);
            DeleteObject(state->inputBrush);
            DeleteObject(state->metricFont);
            DeleteObject(state->cardLabelFont);
            DeleteObject(state->smallFont);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete state;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace ashare
