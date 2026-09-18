#include "stock_screener_view.h"

#include "chart_view.h"
#include "quote_provider.h"
#include "resource_ids.h"
#include "stock_screener.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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

struct ScreenerPendingResult {
    std::vector<StockScreeningResult> rows;
    std::vector<std::string> issues;
    std::string error;
};

struct ScreenerMailbox {
    std::mutex mutex;
    std::optional<ScreenerPendingResult> pending;
    std::atomic_bool alive{true};
    std::atomic<HWND> window{nullptr};
};

struct ScreenerViewState {
    AppState* app = nullptr;
    HWND window = nullptr;
    HWND universeCombo = nullptr;
    HWND limitCombo = nullptr;
    HWND runButton = nullptr;
    HWND addFavoriteButton = nullptr;
    HWND addStrategyButton = nullptr;
    HWND issuesButton = nullptr;
    HWND status = nullptr;
    HWND list = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HFONT metricFont = nullptr;
    HFONT smallFont = nullptr;
    std::vector<StockScreeningResult> results;
    std::vector<std::string> issues;
    std::shared_ptr<ScreenerMailbox> mailbox = std::make_shared<ScreenerMailbox>();
    bool running = false;
};

ScreenerViewState* viewState(HWND window) {
    return reinterpret_cast<ScreenerViewState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
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

void setText(HWND control, const std::wstring& text) {
    int length = GetWindowTextLengthW(control);
    std::wstring current(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, current.data(), length + 1);
    current.resize(static_cast<size_t>(length));
    if (current != text) SetWindowTextW(control, text.c_str());
}

std::wstring amountText(double amount) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(amount >= 100'000'000.0 ? 2 : 0);
    if (amount >= 100'000'000.0) out << amount / 100'000'000.0 << L"亿";
    else if (amount >= 10'000.0) out << amount / 10'000.0 << L"万";
    else out << amount;
    return out.str();
}

std::wstring percentText(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(2) << value << L"%";
    return out.str();
}

void addColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.fmt = LVCFMT_CENTER;
    ListView_InsertColumn(list, index, &column);
}

void configureColumns(HWND list) {
    const wchar_t* titles[] = {
        L"排名", L"代码", L"名称", L"适配度", L"等级", L"最新", L"涨跌幅",
        L"成交额", L"日内振幅", L"20日均振幅", L"历史机会日率", L"趋势偏离", L"说明",
    };
    const int widths[] = {54, 86, 116, 76, 90, 76, 80, 94, 86, 98, 104, 86, 340};
    for (int index = 0; index < static_cast<int>(std::size(titles)); ++index) {
        addColumn(list, index, titles[index], widths[index]);
    }
}

void setSubItem(HWND list, int row, int column, const std::wstring& text) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = column;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    ListView_SetItem(list, &item);
}

void populateResults(ScreenerViewState& state) {
    SendMessageW(state.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state.list);
    for (int index = 0; index < static_cast<int>(state.results.size()); ++index) {
        const auto& result = state.results[static_cast<size_t>(index)];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        std::wstring rank = std::to_wstring(index + 1);
        item.pszText = rank.data();
        ListView_InsertItem(state.list, &item);
        int precision = isFundLikeSymbol(result.quote.symbol) ? 3 : 2;
        setSubItem(state.list, index, 1, utf8ToWide(result.quote.symbol));
        setSubItem(state.list, index, 2, utf8ToWide(result.quote.name));
        setSubItem(state.list, index, 3, std::to_wstring(result.score));
        setSubItem(state.list, index, 4, utf8ToWide(result.grade));
        setSubItem(state.list, index, 5, formatNumber(result.quote.price, precision));
        setSubItem(state.list, index, 6, percentText(result.quote.changePercent));
        setSubItem(state.list, index, 7, amountText(result.quote.amount));
        setSubItem(state.list, index, 8, percentText(result.intradayRangePercent));
        setSubItem(state.list, index, 9, percentText(result.averageDailyAmplitudePercent));
        setSubItem(state.list, index, 10, percentText(result.opportunityDayPercent));
        setSubItem(state.list, index, 11, percentText(result.trendDeviationPercent));
        setSubItem(state.list, index, 12, utf8ToWide(result.reason));
    }
    SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state.list, nullptr, FALSE);
    setText(state.issuesButton,
            L"错误信息 (" + std::to_wstring(state.issues.size()) + L")");
}

std::vector<std::string> selectedUniverse(const ScreenerViewState& state) {
    int selected = static_cast<int>(SendMessageW(state.universeCombo, CB_GETCURSEL, 0, 0));
    if (selected == 1) return liquidEtfCandidates();
    if (selected == 2) return liquidStockCandidates();
    if (selected == 3) {
        std::set<std::string> unique = state.app->favorites;
        unique.insert(state.app->strategySymbols.begin(), state.app->strategySymbols.end());
        for (const auto& [symbol, holding] : state.app->holdings) {
            if (holding.quantity > 0) unique.insert(symbol);
        }
        return {unique.begin(), unique.end()};
    }
    return liquidBalancedCandidates();
}

int selectedLimit(const ScreenerViewState& state) {
    int selected = static_cast<int>(SendMessageW(state.limitCombo, CB_GETCURSEL, 0, 0));
    return selected == 0 ? 10 : selected == 2 ? 30 : 20;
}

void startScreening(ScreenerViewState& state) {
    if (state.running) return;
    std::vector<std::string> universe = selectedUniverse(state);
    if (universe.empty()) {
        setText(state.status, L"当前范围没有可分析标的，请先添加自选、持仓或策略观察。 ");
        return;
    }
    const int limit = selectedLimit(state);
    state.running = true;
    EnableWindow(state.runButton, FALSE);
    setText(state.status, L"正在获取实时快照并分析近20日真实日线...");
    logger().write("INFO", "Stock screener started universe=" +
                               std::to_string(universe.size()) +
                               " limit=" + std::to_string(limit));
    auto mailbox = state.mailbox;
    std::thread([mailbox, universe = std::move(universe), limit]() mutable {
        ScreenerPendingResult pending;
        try {
            QuoteProvider provider;
            std::vector<Quote> quotes = provider.fetchQuotes(universe);
            quotes.erase(std::remove_if(quotes.begin(), quotes.end(), [](const Quote& quote) {
                             return quote.price <= 0.0 || quote.previousClose <= 0.0 ||
                                    quote.amount <= 0.0;
                         }), quotes.end());
            std::stable_sort(quotes.begin(), quotes.end(), [](const Quote& left, const Quote& right) {
                return left.amount > right.amount;
            });
            if (quotes.size() > static_cast<size_t>(limit)) {
                quotes.resize(static_cast<size_t>(limit));
            }
            for (const auto& quote : quotes) {
                if (!mailbox->alive.load()) return;
                try {
                    auto daily = provider.fetchKLines(quote.symbol, 101, 30);
                    auto result = evaluateT0Suitability(quote, daily);
                    if (result.score > 0) pending.rows.push_back(std::move(result));
                    else pending.issues.push_back(quote.symbol + "：" + result.reason);
                } catch (const std::exception& error) {
                    pending.issues.push_back(quote.symbol + "：" + error.what());
                }
            }
            std::stable_sort(pending.rows.begin(), pending.rows.end(),
                             [](const StockScreeningResult& left,
                                const StockScreeningResult& right) {
                                 if (left.score != right.score) return left.score > right.score;
                                 return left.quote.amount > right.quote.amount;
                             });
            if (quotes.empty()) pending.error = "实时行情未返回有效候选标的。";
            else if (pending.rows.empty()) pending.error = "候选标的日线均不完整，请查看错误信息。";
        } catch (const std::exception& error) {
            pending.error = error.what();
        }
        if (!mailbox->alive.load()) return;
        {
            std::lock_guard<std::mutex> lock(mailbox->mutex);
            mailbox->pending = std::move(pending);
        }
        HWND window = mailbox->window.load();
        if (window) PostMessageW(window, WM_APP_SCREENER_RESULT, 0, 0);
    }).detach();
}

const StockScreeningResult* selectedResult(const ScreenerViewState& state) {
    int selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    if (selected < 0 || static_cast<size_t>(selected) >= state.results.size()) return nullptr;
    return &state.results[static_cast<size_t>(selected)];
}

void addSelectedFavorite(ScreenerViewState& state) {
    const auto* result = selectedResult(state);
    if (!result) {
        setText(state.status, L"请先在结果表中选择一个标的。 ");
        return;
    }
    state.app->favorites.insert(result->quote.symbol);
    state.app->quoteCache[result->quote.symbol] = result->quote;
    state.app->symbolNames[result->quote.symbol] = result->quote.name;
    state.app->store.save(state.app->favorites);
    setText(state.status, L"已加入自选：" + utf8ToWide(result->quote.symbol) + L"  " +
                              utf8ToWide(result->quote.name));
}

void addSelectedStrategy(ScreenerViewState& state) {
    const auto* result = selectedResult(state);
    if (!result) {
        setText(state.status, L"请先在结果表中选择一个标的。 ");
        return;
    }
    if (state.app->strategySymbols.count(result->quote.symbol) == 0 &&
        state.app->strategySymbols.size() >= 10) {
        setText(state.status, L"策略观察最多保留 10 个标的，请先移出一个标的。 ");
        return;
    }
    // 策略观察沿用“观察标的同时属于自选”的既有约束。
    state.app->favorites.insert(result->quote.symbol);
    state.app->strategySymbols.insert(result->quote.symbol);
    state.app->quoteCache[result->quote.symbol] = result->quote;
    state.app->symbolNames[result->quote.symbol] = result->quote.name;
    state.app->store.save(state.app->favorites);
    state.app->strategyStore.save(state.app->strategySymbols);
    setText(state.status, L"已加入策略观察：" + utf8ToWide(result->quote.symbol) + L"  " +
                              utf8ToWide(result->quote.name));
}

void showIssues(const ScreenerViewState& state) {
    std::wstring text;
    if (state.issues.empty()) {
        text = L"本次筛选没有行情错误。";
    } else {
        size_t count = std::min<size_t>(state.issues.size(), 30);
        for (size_t index = 0; index < count; ++index) {
            text += utf8ToWide(state.issues[index]) + L"\r\n";
        }
        if (state.issues.size() > count) text += L"更多错误请查看运行日志。";
    }
    MessageBoxW(state.window, text.c_str(), L"选股行情错误", MB_OK | MB_ICONINFORMATION);
}

void layoutControls(ScreenerViewState& state, int width, int height) {
    constexpr int margin = 10;
    constexpr int firstRowY = 24;
    constexpr int secondRowY = 70;
    MoveWindow(state.universeCombo, 92, firstRowY, 220, 240, TRUE);
    MoveWindow(state.limitCombo, 398, firstRowY, 130, 180, TRUE);
    MoveWindow(state.runButton, 544, firstRowY, 104, 36, TRUE);
    MoveWindow(state.addFavoriteButton, 24, secondRowY, 104, 36, TRUE);
    MoveWindow(state.addStrategyButton, 138, secondRowY, 118, 36, TRUE);
    MoveWindow(state.issuesButton, 266, secondRowY, 104, 36, TRUE);
    MoveWindow(state.status, 390, secondRowY + 6, std::max(180, width - 414), 24, TRUE);
    MoveWindow(state.list, margin, 188, std::max(100, width - margin * 2),
               std::max(100, height - 198), TRUE);
}

void paintView(HWND window, ScreenerViewState& state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    ThemePalette palette = themePalette();
    FillRect(dc, &client, state.windowBrush);
    drawRoundedPanel(dc, RECT{10, 10, client.right - 10, 126},
                     palette.surface, palette.border);
    drawRoundedPanel(dc, RECT{10, 136, client.right - 10, 178},
                     palette.surface, palette.border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, palette.text);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, state.app->font));
    RECT label{24, 24, 88, 60};
    DrawTextW(dc, L"候选范围", -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    label = RECT{326, 24, 394, 60};
    DrawTextW(dc, L"分析数量", -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int qualified = static_cast<int>(std::count_if(
        state.results.begin(), state.results.end(),
        [](const StockScreeningResult& row) { return row.qualified; }));
    int best = state.results.empty() ? 0 : state.results.front().score;
    std::wstring summary = state.running
        ? L"正在分析候选行情..."
        : L"已分析  " + std::to_wstring(state.results.size()) +
              L"    适合观察  " + std::to_wstring(qualified) +
              L"    最高适配度  " + (best > 0 ? std::to_wstring(best) : L"--") +
              L"    评分口径：流动性 / 振幅 / 机会日 / 反转特征 / 趋势风险";
    SelectObject(dc, state.metricFont);
    SetTextColor(dc, best >= 75 ? palette.accent : palette.text);
    RECT summaryRect{24, 136, client.right - 24, 178};
    DrawTextW(dc, summary.c_str(), -1, &summaryRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    EndPaint(window, &paint);
}

}  // namespace

LRESULT CALLBACK StockScreenerViewProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* app = reinterpret_cast<AppState*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        auto* state = new ScreenerViewState();
        state->app = app;
        state->window = window;
        state->mailbox->window.store(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        ThemePalette palette = themePalette();
        state->windowBrush = CreateSolidBrush(palette.window);
        state->surfaceBrush = CreateSolidBrush(palette.surface);
        state->metricFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
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
        state->universeCombo = combo(IDC_SCREENER_UNIVERSE);
        for (const wchar_t* value : {L"高流动性综合池", L"高流动性ETF", L"高流动性股票",
                                     L"自选 / 持仓 / 观察"}) {
            SendMessageW(state->universeCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(value));
        }
        SendMessageW(state->universeCombo, CB_SETCURSEL, 0, 0);
        state->limitCombo = combo(IDC_SCREENER_LIMIT);
        for (const wchar_t* value : {L"分析 10 只", L"分析 20 只", L"分析 30 只"}) {
            SendMessageW(state->limitCombo, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(value));
        }
        SendMessageW(state->limitCombo, CB_SETCURSEL, 1, 0);
        auto button = [&](const wchar_t* text, int id) {
            HWND control = CreateWindowW(L"BUTTON", text,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         0, 0, 0, 0, window, reinterpret_cast<HMENU>(id),
                                         app->instance, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
            return control;
        };
        state->runButton = button(L"开始选股", IDC_SCREENER_RUN);
        state->addFavoriteButton = button(L"加入自选", IDC_SCREENER_ADD_FAVORITE);
        state->addStrategyButton = button(L"加入策略观察", IDC_SCREENER_ADD_STRATEGY);
        state->issuesButton = button(L"错误信息 (0)", IDC_SCREENER_ISSUES);
        state->status = CreateWindowW(
            L"STATIC", L"选择候选范围后开始分析；结果是当前 T0 算法适配度，不构成投资建议。",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(IDC_SCREENER_STATUS), app->instance, nullptr);
        SendMessageW(state->status, WM_SETFONT,
                     reinterpret_cast<WPARAM>(state->smallFont), TRUE);
        state->list = CreateWindowExW(
            0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_SCREENER_LIST),
            app->instance, nullptr);
        SendMessageW(state->list, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
        ListView_SetExtendedListViewStyle(state->list,
                                          LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        ListView_SetBkColor(state->list, palette.surface);
        ListView_SetTextBkColor(state->list, palette.surface);
        ListView_SetTextColor(state->list, palette.text);
        setExplorerControlTheme(state->list);
        setExplorerControlTheme(ListView_GetHeader(state->list));
        SetWindowSubclass(ListView_GetHeader(state->list), HeaderProc, 5,
                          reinterpret_cast<DWORD_PTR>(app));
        configureColumns(state->list);
        RECT client{};
        GetClientRect(window, &client);
        layoutControls(*state, client.right, client.bottom);
        return 0;
    }

    ScreenerViewState* state = viewState(window);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
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
                drawModalButton(*item, item->CtlID == IDC_SCREENER_RUN);
                return TRUE;
            }
            if (item->CtlType == ODT_COMBOBOX) return drawThemedComboItem(*item);
        }
        break;
    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wParam) == IDC_SCREENER_RUN && HIWORD(wParam) == BN_CLICKED) {
            startScreening(*state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SCREENER_ADD_FAVORITE && HIWORD(wParam) == BN_CLICKED) {
            addSelectedFavorite(*state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SCREENER_ADD_STRATEGY && HIWORD(wParam) == BN_CLICKED) {
            addSelectedStrategy(*state);
            return 0;
        }
        if (LOWORD(wParam) == IDC_SCREENER_ISSUES && HIWORD(wParam) == BN_CLICKED) {
            showIssues(*state);
            return 0;
        }
        break;
    case WM_NOTIFY:
        if (state) {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == state->list && header->code == NM_CUSTOMDRAW) {
                auto* custom = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                if (custom->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (custom->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                if (custom->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                    size_t row = static_cast<size_t>(custom->nmcd.dwItemSpec);
                    int column = custom->iSubItem;
                    ThemePalette palette = themePalette();
                    custom->clrTextBk = palette.surface;
                    custom->clrText = palette.text;
                    if (row < state->results.size()) {
                        const auto& result = state->results[row];
                        if (column == 3 || column == 4) {
                            custom->clrText = result.qualified ? palette.accent : palette.muted;
                        } else if (column == 6) {
                            custom->clrText = result.quote.changePercent > 0.0
                                                  ? palette.up
                                                  : result.quote.changePercent < 0.0
                                                        ? palette.down : palette.text;
                        }
                    }
                    return CDRF_NEWFONT;
                }
            }
        }
        break;
    case WM_APP_SCREENER_RESULT:
        if (state) {
            ScreenerPendingResult pending;
            {
                std::lock_guard<std::mutex> lock(state->mailbox->mutex);
                if (!state->mailbox->pending) return 0;
                pending = std::move(*state->mailbox->pending);
                state->mailbox->pending.reset();
            }
            state->running = false;
            EnableWindow(state->runButton, TRUE);
            state->results = std::move(pending.rows);
            state->issues = std::move(pending.issues);
            for (const auto& row : state->results) {
                state->app->quoteCache[row.quote.symbol] = row.quote;
                state->app->symbolNames[row.quote.symbol] = row.quote.name;
            }
            populateResults(*state);
            if (!pending.error.empty()) {
                state->issues.push_back("筛选任务：" + pending.error);
                setText(state->status, L"筛选失败：" + utf8ToWide(pending.error));
                logger().write("ERROR", "Stock screener failed: " + pending.error);
            } else {
                int qualified = static_cast<int>(std::count_if(
                    state->results.begin(), state->results.end(),
                    [](const StockScreeningResult& row) { return row.qualified; }));
                setText(state->status,
                        L"筛选完成：分析 " + std::to_wstring(state->results.size()) +
                            L" 只，适合观察 " + std::to_wstring(qualified) +
                            L" 只。请结合持仓、费用和风险复核。 ");
                logger().write("INFO", "Stock screener completed rows=" +
                                           std::to_string(state->results.size()) +
                                           " qualified=" + std::to_string(qualified) +
                                           " issues=" + std::to_string(state->issues.size()));
            }
            setText(state->issuesButton,
                    L"错误信息 (" + std::to_wstring(state->issues.size()) + L")");
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        if (state) {
            state->mailbox->alive.store(false);
            state->mailbox->window.store(nullptr);
            DeleteObject(state->windowBrush);
            DeleteObject(state->surfaceBrush);
            DeleteObject(state->metricFont);
            DeleteObject(state->smallFont);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete state;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace ashare
