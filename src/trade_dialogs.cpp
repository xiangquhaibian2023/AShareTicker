#include "dialogs.h"

#include "dashboard_view.h"
#include "main_window.h"
#include "market_controller.h"
#include "resource_ids.h"
#include "trade_ledger.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ashare {

namespace {

struct TradeManagerContext {
    AppState* state = nullptr;
    bool strategyScope = false;
    HFONT font = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HWND list = nullptr;
    HWND symbol = nullptr;
    HWND side = nullptr;
    HWND time = nullptr;
    HWND quantity = nullptr;
    HWND price = nullptr;
    HWND commission = nullptr;
    HWND stampDuty = nullptr;
    HWND transferFee = nullptr;
    HWND otherFees = nullptr;
    std::vector<std::string> symbols;
    std::vector<std::string> visibleIds;
};

struct FundsDialogContext {
    AppState* state = nullptr;
    HFONT font = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HWND totalAssets = nullptr;
    HWND availableCash = nullptr;
};

struct HoldingDialogContext {
    AppState* state = nullptr;
    bool strategyScope = false;
    HFONT font = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH inputBrush = nullptr;
    HWND symbol = nullptr;
    HWND quantity = nullptr;
    HWND cost = nullptr;
    std::wstring symbolValue;
    std::wstring quantityValue;
    std::wstring costValue;
};

TradeManagerContext* tradeContext(HWND window) {
    return reinterpret_cast<TradeManagerContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

FundsDialogContext* fundsContext(HWND window) {
    return reinterpret_cast<FundsDialogContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

HoldingDialogContext* holdingContext(HWND window) {
    return reinterpret_cast<HoldingDialogContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring currentMinuteText() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[32]{};
    swprintf_s(value, L"%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute);
    return value;
}

std::optional<long long> integerValue(HWND edit) {
    try {
        std::wstring text = getEditText(edit);
        size_t end = 0;
        long long value = std::stoll(text, &end);
        return end == text.size() ? std::optional<long long>(value) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<double> numberValue(HWND edit) {
    try {
        std::wstring text = getEditText(edit);
        size_t end = 0;
        double value = std::stod(text, &end);
        return end == text.size() && std::isfinite(value)
                   ? std::optional<double>(value) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void addListColumn(HWND list, int index, const wchar_t* text, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(text);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

std::vector<TradeRecord>& records(TradeManagerContext& context) {
    return context.strategyScope ? context.state->strategyTrades : context.state->holdingTrades;
}

const TradeRecord* findRecord(const TradeManagerContext& context, const std::string& id) {
    const auto& source = context.strategyScope ? context.state->strategyTrades
                                               : context.state->holdingTrades;
    auto found = std::find_if(source.begin(), source.end(), [&](const TradeRecord& trade) {
        return trade.id == id;
    });
    return found == source.end() ? nullptr : &*found;
}

void selectComboValue(HWND combo, const std::vector<std::string>& values,
                      const std::string& value) {
    auto found = std::find(values.begin(), values.end(), value);
    if (found != values.end()) {
        SendMessageW(combo, CB_SETCURSEL, std::distance(values.begin(), found), 0);
    }
}

void clearTradeEditor(TradeManagerContext& context) {
    if (!context.state->currentSymbol.empty()) {
        selectComboValue(context.symbol, context.symbols, context.state->currentSymbol);
    }
    SendMessageW(context.side, CB_SETCURSEL, 0, 0);
    SetWindowTextW(context.time, currentMinuteText().c_str());
    SetWindowTextW(context.quantity, L"");
    SetWindowTextW(context.price, L"");
    SetWindowTextW(context.commission, L"5.00");
    SetWindowTextW(context.stampDuty, L"0.00");
    SetWindowTextW(context.transferFee, L"0.00");
    SetWindowTextW(context.otherFees, L"0.00");
    ListView_SetItemState(context.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
}

void refreshTradeList(TradeManagerContext& context) {
    SendMessageW(context.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(context.list);
    context.visibleIds.clear();
    std::vector<TradeRecord> display;
    for (const auto& trade : records(context)) {
        if (tradeDateToken(trade.time) != currentDateToken()) continue;
        if (context.strategyScope && trade.strategyId != context.state->activeStrategyId) continue;
        display.push_back(trade);
    }
    std::stable_sort(display.begin(), display.end(), [](const auto& left, const auto& right) {
        return left.time != right.time ? left.time > right.time : left.id > right.id;
    });
    for (int row = 0; row < static_cast<int>(display.size()); ++row) {
        const auto& trade = display[static_cast<size_t>(row)];
        context.visibleIds.push_back(trade.id);
        std::vector<std::wstring> values = {
            utf8ToWide(trade.time), utf8ToWide(trade.symbol),
            trade.side == SignalType::Buy ? L"买入" : L"卖出",
            std::to_wstring(trade.quantity), formatNumber(trade.price, 3),
            formatNumber(trade.commission), formatNumber(trade.stampDuty),
            formatNumber(trade.transferFee), formatNumber(trade.otherFees),
            trade.source == TradeSource::Manual ? L"手工录入" : L"算法模拟",
        };
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = values[0].data();
        ListView_InsertItem(context.list, &item);
        for (int column = 1; column < static_cast<int>(values.size()); ++column) {
            ListView_SetItemText(context.list, row, column, values[static_cast<size_t>(column)].data());
        }
    }
    SendMessageW(context.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(context.list, nullptr, TRUE);
}

void loadSelectedTrade(TradeManagerContext& context) {
    int row = ListView_GetNextItem(context.list, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(context.visibleIds.size())) return;
    const TradeRecord* trade = findRecord(context, context.visibleIds[static_cast<size_t>(row)]);
    if (!trade) return;
    selectComboValue(context.symbol, context.symbols, trade->symbol);
    SendMessageW(context.side, CB_SETCURSEL, trade->side == SignalType::Buy ? 0 : 1, 0);
    SetWindowTextW(context.time, utf8ToWide(trade->time).c_str());
    SetWindowTextW(context.quantity, std::to_wstring(trade->quantity).c_str());
    SetWindowTextW(context.price, formatNumber(trade->price, 3).c_str());
    SetWindowTextW(context.commission, formatNumber(trade->commission).c_str());
    SetWindowTextW(context.stampDuty, formatNumber(trade->stampDuty).c_str());
    SetWindowTextW(context.transferFee, formatNumber(trade->transferFee).c_str());
    SetWindowTextW(context.otherFees, formatNumber(trade->otherFees).c_str());
}

std::optional<TradeRecord> readTradeEditor(TradeManagerContext& context, HWND window) {
    int symbolIndex = static_cast<int>(SendMessageW(context.symbol, CB_GETCURSEL, 0, 0));
    int sideIndex = static_cast<int>(SendMessageW(context.side, CB_GETCURSEL, 0, 0));
    auto quantity = integerValue(context.quantity);
    auto price = numberValue(context.price);
    auto commission = numberValue(context.commission);
    auto stampDuty = numberValue(context.stampDuty);
    auto transferFee = numberValue(context.transferFee);
    auto otherFees = numberValue(context.otherFees);
    std::string time = wideToUtf8(getEditText(context.time));
    bool valid = symbolIndex >= 0 && symbolIndex < static_cast<int>(context.symbols.size()) &&
                 (sideIndex == 0 || sideIndex == 1) && quantity && *quantity > 0 &&
                 price && *price > 0.0 && commission && *commission >= 0.0 &&
                 stampDuty && *stampDuty >= 0.0 && transferFee && *transferFee >= 0.0 &&
                 otherFees && *otherFees >= 0.0 && tradeDateToken(time) == currentDateToken() &&
                 time.size() >= 16;
    if (!valid) {
        MessageBoxW(window, L"请检查标的、当日时间、方向、数量、价格及各项费用。",
                    L"输入错误", MB_ICONWARNING);
        return std::nullopt;
    }
    TradeRecord trade;
    trade.id = generateTradeRecordId();
    trade.strategyId = context.strategyScope ? context.state->activeStrategyId : std::string{};
    trade.symbol = context.symbols[static_cast<size_t>(symbolIndex)];
    trade.time = time.substr(0, 16);
    trade.side = sideIndex == 0 ? SignalType::Buy : SignalType::Sell;
    trade.quantity = *quantity;
    trade.price = *price;
    trade.commission = *commission;
    trade.stampDuty = *stampDuty;
    trade.transferFee = *transferFee;
    trade.otherFees = *otherFees;
    trade.source = TradeSource::Manual;
    return trade;
}

bool validateAndCommit(TradeManagerContext& context, std::vector<TradeRecord> candidate,
                       HWND window) {
    if (!context.strategyScope) {
        std::map<std::string, Holding> holdings = context.state->holdings;
        for (auto& [symbol, holding] : holdings) {
            std::string error;
            if (!recalculateHoldingFromTrades(holding, candidate, currentDateToken(), &error)) {
                MessageBoxW(window, utf8ToWide(symbol + "：" + error).c_str(), L"成交校验失败",
                            MB_ICONWARNING);
                return false;
            }
        }
        context.state->holdings = std::move(holdings);
        context.state->holdingTrades = std::move(candidate);
        context.state->holdingStore.save(context.state->holdings);
        context.state->holdingTradeStore.save(context.state->holdingTrades);
        populateHoldings(*context.state);
    } else {
        for (const auto& symbol : context.symbols) {
            auto holding = context.state->holdings.find(symbol);
            long long openingQuantity = 0;
            double openingCost = 0.0;
            if (holding != context.state->holdings.end()) {
                openingQuantity = holding->second.tradeBaseDate == currentDateToken()
                                      ? holding->second.tradeBaseQuantity : holding->second.quantity;
                openingCost = holding->second.tradeBaseDate == currentDateToken()
                                  ? holding->second.tradeBaseCost : holding->second.cost;
            }
            auto effective = effectiveStrategyTrades(candidate, context.state->activeStrategyId,
                                                      symbol, currentDateToken());
            auto snapshot = calculateTradeLedger(openingQuantity, openingCost, effective);
            if (!snapshot.valid) {
                MessageBoxW(window, utf8ToWide(symbol + "：" + snapshot.error).c_str(),
                            L"策略成交校验失败", MB_ICONWARNING);
                return false;
            }
        }
        context.state->strategyTrades = std::move(candidate);
        context.state->strategyTradeStore.save(context.state->strategyTrades);
        populateStrategySignals(*context.state);
    }
    populateTradeDetails(*context.state);
    context.state->hoveredTradeMarker.reset();
    InvalidateRect(context.state->kline, nullptr, FALSE);
    return true;
}

void runModalWindow(HWND owner, HWND dialog) {
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
}

}  // namespace

LRESULT CALLBACK TradeDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* context = reinterpret_cast<TradeManagerContext*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        auto label = [&](const wchar_t* text, int x, int y, int width) {
            HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                         x, y, width, 22, window, nullptr, nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        };
        auto edit = [&](int id, int x, int y, int width, const wchar_t* value = L"") {
            HWND control = CreateWindowExW(0, L"EDIT", value,
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                               ES_CENTER | ES_MULTILINE,
                                           x, y, width, 30, window, reinterpret_cast<HMENU>(id),
                                           nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
            SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(7, 7));
            installRoundedInputTheme(control);
            return control;
        };
        context->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                        16, 16, 892, 250, window,
                                        reinterpret_cast<HMENU>(IDC_TRADE_LIST), nullptr, nullptr);
        ListView_SetExtendedListViewStyle(context->list,
                                          LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        const wchar_t* headers[] = {L"时间", L"代码", L"方向", L"数量", L"成交价",
                                    L"佣金", L"印花税", L"过户费", L"其他费用", L"来源"};
        const int widths[] = {122, 82, 56, 68, 72, 62, 62, 62, 70, 76};
        for (int index = 0; index < 10; ++index) addListColumn(context->list, index, headers[index], widths[index]);
        setExplorerControlTheme(context->list);
        SendMessageW(context->list, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);

        label(L"标的", 18, 286, 48);
        context->symbol = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                              CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                          66, 278, 132, 220, window,
                                          reinterpret_cast<HMENU>(IDC_TRADE_SYMBOL), nullptr, nullptr);
        label(L"方向", 216, 286, 48);
        context->side = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                        264, 278, 88, 160, window,
                                        reinterpret_cast<HMENU>(IDC_TRADE_SIDE), nullptr, nullptr);
        SendMessageW(context->side, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"买入"));
        SendMessageW(context->side, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"卖出"));
        label(L"时间", 370, 286, 48);
        context->time = edit(IDC_TRADE_TIME, 418, 278, 158, currentMinuteText().c_str());
        label(L"数量", 594, 286, 48);
        context->quantity = edit(IDC_TRADE_QUANTITY, 642, 278, 104);
        label(L"成交价", 764, 286, 56);
        context->price = edit(IDC_TRADE_PRICE, 820, 278, 88);

        label(L"佣金", 18, 336, 48);
        context->commission = edit(IDC_TRADE_COMMISSION, 66, 328, 100, L"5.00");
        label(L"印花税", 184, 336, 56);
        context->stampDuty = edit(IDC_TRADE_STAMP_DUTY, 240, 328, 100, L"0.00");
        label(L"过户费", 358, 336, 56);
        context->transferFee = edit(IDC_TRADE_TRANSFER_FEE, 414, 328, 100, L"0.00");
        label(L"其他费用", 532, 336, 68);
        context->otherFees = edit(IDC_TRADE_OTHER_FEES, 600, 328, 100, L"0.00");

        auto button = [&](const wchar_t* text, int id, int x, int width) {
            HWND control = CreateWindowW(L"BUTTON", text,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                         x, 386, width, 34, window,
                                         reinterpret_cast<HMENU>(id), nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        };
        button(L"新增", IDC_TRADE_ADD, 542, 82);
        button(L"修改", IDC_TRADE_UPDATE, 632, 82);
        button(L"删除", IDC_TRADE_DELETE, 722, 82);
        button(L"关闭", IDCANCEL, 812, 82);
        for (const auto& symbol : context->symbols) {
            std::wstring value = utf8ToWide(symbol);
            SendMessageW(context->symbol, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value.c_str()));
        }
        SendMessageW(context->symbol, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        SendMessageW(context->side, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        installRoundedComboTheme(context->symbol);
        installRoundedComboTheme(context->side);
        clearTradeEditor(*context);
        refreshTradeList(*context);
        setImmersiveDarkTitleBar(window);
        return 0;
    }
    TradeManagerContext* context = tradeContext(window);
    switch (message) {
    case WM_ERASEBKGND:
        if (context) {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, context->windowBrush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (context) {
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), themePalette().text);
            return reinterpret_cast<LRESULT>(context->windowBrush);
        }
        break;
    case WM_CTLCOLOREDIT:
        if (context) {
            SetBkColor(reinterpret_cast<HDC>(wParam), themePalette().surface);
            SetTextColor(reinterpret_cast<HDC>(wParam), themePalette().text);
            return reinterpret_cast<LRESULT>(context->surfaceBrush);
        }
        break;
    case WM_DRAWITEM:
        {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_BUTTON) {
            drawModalButton(item, item.CtlID == IDC_TRADE_ADD);
            return TRUE;
            }
            if (item.CtlType == ODT_COMBOBOX) {
                return drawThemedComboItem(item);
            }
        }
        break;
    case WM_NOTIFY:
        if (context && reinterpret_cast<LPNMHDR>(lParam)->idFrom == IDC_TRADE_LIST &&
            reinterpret_cast<LPNMHDR>(lParam)->code == LVN_ITEMCHANGED) {
            auto* item = reinterpret_cast<LPNMLISTVIEW>(lParam);
            if ((item->uNewState & LVIS_SELECTED) != 0) loadSelectedTrade(*context);
        }
        break;
    case WM_COMMAND:
        if (!context) break;
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDC_TRADE_ADD) {
            auto trade = readTradeEditor(*context, window);
            if (trade) {
                auto candidate = records(*context);
                candidate.push_back(*trade);
                if (validateAndCommit(*context, std::move(candidate), window)) {
                    refreshTradeList(*context);
                    clearTradeEditor(*context);
                }
            }
            return 0;
        }
        if (LOWORD(wParam) == IDC_TRADE_UPDATE || LOWORD(wParam) == IDC_TRADE_DELETE) {
            int row = ListView_GetNextItem(context->list, -1, LVNI_SELECTED);
            if (row < 0 || row >= static_cast<int>(context->visibleIds.size())) {
                MessageBoxW(window, L"请先选择一笔成交。", L"提示", MB_ICONINFORMATION);
                return 0;
            }
            const std::string id = context->visibleIds[static_cast<size_t>(row)];
            const TradeRecord* selected = findRecord(*context, id);
            if (!selected || selected->source != TradeSource::Manual) {
                MessageBoxW(window, L"算法模拟成交为只读记录；录入手工成交后会自动覆盖其计算口径。",
                            L"只读记录", MB_ICONINFORMATION);
                return 0;
            }
            auto candidate = records(*context);
            auto target = std::find_if(candidate.begin(), candidate.end(),
                                       [&](const TradeRecord& trade) { return trade.id == id; });
            if (target == candidate.end()) return 0;
            if (LOWORD(wParam) == IDC_TRADE_DELETE) {
                candidate.erase(target);
            } else {
                auto replacement = readTradeEditor(*context, window);
                if (!replacement) return 0;
                replacement->id = id;
                *target = *replacement;
            }
            if (validateAndCommit(*context, std::move(candidate), window)) {
                refreshTradeList(*context);
                clearTradeEditor(*context);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void editTodayTrades(AppState& state) {
    TradeManagerContext context;
    context.state = &state;
    context.strategyScope = state.detailView == DetailView::Strategy;
    if (context.strategyScope) {
        context.symbols.assign(state.strategySymbols.begin(), state.strategySymbols.end());
    } else {
        for (const auto& [symbol, holding] : state.holdings) {
            (void)holding;
            context.symbols.push_back(symbol);
        }
    }
    if (context.symbols.empty()) {
        MessageBoxW(state.window, context.strategyScope
                                      ? L"请先添加策略观察标的。" : L"请先添加持仓。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    context.font = state.font;
    ThemePalette palette = themePalette();
    context.windowBrush = CreateSolidBrush(palette.window);
    context.surfaceBrush = CreateSolidBrush(palette.surface);
    RECT owner{};
    GetWindowRect(state.window, &owner);
    constexpr int width = 940;
    constexpr int height = 480;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"AShareTradeDialog",
        context.strategyScope ? L"策略分笔成交管理" : L"持仓分笔成交管理",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        owner.left + (owner.right - owner.left - width) / 2,
        owner.top + (owner.bottom - owner.top - height) / 2,
        width, height, state.window, nullptr, state.instance, &context);
    if (!dialog) {
        DeleteObject(context.windowBrush);
        DeleteObject(context.surfaceBrush);
        throw std::runtime_error("无法打开分笔成交管理窗口。");
    }
    runModalWindow(state.window, dialog);
    DeleteObject(context.windowBrush);
    DeleteObject(context.surfaceBrush);
    populateDetailView(state);
}

LRESULT CALLBACK HoldingDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* context = reinterpret_cast<HoldingDialogContext*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));

        auto label = [&](const wchar_t* text, int y) {
            HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                         28, y, 112, 26, window, nullptr, nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        };
        auto edit = [&](int id, int y, const std::wstring& value, DWORD extraStyle) {
            HWND control = CreateWindowExW(
                0, L"EDIT", value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER |
                    ES_MULTILINE | extraStyle,
                150, y - 5, 270, 36, window, reinterpret_cast<HMENU>(id), nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
            SendMessageW(control, EM_SETLIMITTEXT, 32, 0);
            installRoundedInputTheme(control);
            return control;
        };

        HWND heading = CreateWindowW(
            L"STATIC", context->strategyScope ? L"编辑策略底仓" : L"编辑持仓基准",
            WS_CHILD | WS_VISIBLE, 28, 18, 392, 28, window, nullptr, nullptr, nullptr);
        SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        label(L"证券代码", 70);
        label(context->strategyScope ? L"底仓数量" : L"持仓数量", 124);
        label(L"持仓成本", 178);
        context->symbol = edit(IDC_HOLDING_DIALOG_SYMBOL, 70, context->symbolValue, 0);
        context->quantity = edit(IDC_HOLDING_DIALOG_QUANTITY, 124,
                                 context->quantityValue, ES_NUMBER);
        context->cost = edit(IDC_HOLDING_DIALOG_COST, 178, context->costValue, 0);
        if (context->strategyScope) {
            SendMessageW(context->symbol, EM_SETREADONLY, TRUE, 0);
        }

        HWND hint = CreateWindowW(L"STATIC", L"数量须大于 0，成本保留三位小数。",
                                  WS_CHILD | WS_VISIBLE, 150, 218, 270, 24,
                                  window, nullptr, nullptr, nullptr);
        SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        HWND cancel = CreateWindowW(L"BUTTON", L"取消",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    242, 258, 84, 36, window,
                                    reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        HWND save = CreateWindowW(L"BUTTON", L"保存",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  336, 258, 84, 36, window,
                                  reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        SendMessageW(save, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        setImmersiveDarkTitleBar(window);
        return 0;
    }

    HoldingDialogContext* context = holdingContext(window);
    switch (message) {
    case WM_ERASEBKGND:
        if (context) {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, context->windowBrush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (context) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, themePalette().text);
            return reinterpret_cast<LRESULT>(context->windowBrush);
        }
        break;
    case WM_CTLCOLOREDIT:
        if (context) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, themePalette().surfaceAlt);
            SetTextColor(dc, themePalette().text);
            return reinterpret_cast<LRESULT>(context->inputBrush);
        }
        break;
    case WM_DRAWITEM:
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            drawModalButton(item, item.CtlID == IDOK);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (!context) {
            break;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            std::string symbol = trim(wideToUtf8(getEditText(context->symbol)));
            auto quantity = integerValue(context->quantity);
            auto cost = numberValue(context->cost);
            if (!quantity || !cost) {
                MessageBoxW(window, L"持仓数量和成本必须是有效数字。",
                            L"输入错误", MB_ICONWARNING);
                return 0;
            }
            try {
                if (saveHoldingValues(*context->state, symbol, *quantity, *cost, window)) {
                    DestroyWindow(window);
                }
            } catch (const std::exception& error) {
                MessageBoxW(window, utf8ToWide(error.what()).c_str(), L"保存失败", MB_ICONERROR);
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void editHolding(AppState& state) {
    HoldingDialogContext context;
    context.state = &state;
    context.strategyScope = state.detailView == DetailView::Strategy;
    std::string symbol = state.currentSymbol;
    if (symbol.empty()) {
        auto normalized = normalizeSymbol(wideToUtf8(getEditText(state.edit)));
        if (normalized) {
            symbol = *normalized;
        }
    }
    if (context.strategyScope && symbol.empty()) {
        MessageBoxW(state.window, L"请先选择要编辑底仓的策略标的。",
                    L"提示", MB_ICONINFORMATION);
        return;
    }
    context.symbolValue = utf8ToWide(symbol);
    auto holding = state.holdings.find(symbol);
    if (holding != state.holdings.end()) {
        long long quantity = holding->second.tradeBaseDate == currentDateToken()
                                 ? holding->second.tradeBaseQuantity
                                 : holding->second.quantity;
        double cost = holding->second.tradeBaseDate == currentDateToken()
                          ? holding->second.tradeBaseCost
                          : holding->second.cost;
        context.quantityValue = std::to_wstring(quantity);
        context.costValue = formatNumber(cost, 3);
    }
    context.font = state.font;
    ThemePalette palette = themePalette();
    context.windowBrush = CreateSolidBrush(palette.window);
    context.inputBrush = CreateSolidBrush(palette.surfaceAlt);
    RECT owner{};
    GetWindowRect(state.window, &owner);
    constexpr int width = 470;
    constexpr int height = 350;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"AShareHoldingDialog",
        context.strategyScope ? L"编辑策略底仓" : L"编辑持仓",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        owner.left + (owner.right - owner.left - width) / 2,
        owner.top + (owner.bottom - owner.top - height) / 2,
        width, height, state.window, nullptr, state.instance, &context);
    if (!dialog) {
        DeleteObject(context.windowBrush);
        DeleteObject(context.inputBrush);
        throw std::runtime_error("无法打开持仓编辑窗口。");
    }
    runModalWindow(state.window, dialog);
    DeleteObject(context.windowBrush);
    DeleteObject(context.inputBrush);
    populateDetailView(state);
    updateHoldingSummary(state);
}

LRESULT CALLBACK FundsDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* context = reinterpret_cast<FundsDialogContext*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        auto label = [&](const wchar_t* text, int y) {
            HWND control = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                         24, y, 120, 24, window, nullptr, nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        };
        auto edit = [&](int id, int y, double value) {
            HWND control = CreateWindowExW(0, L"EDIT", formatNumber(value).c_str(),
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                               ES_CENTER | ES_MULTILINE,
                                           150, y - 5, 240, 32, window,
                                           reinterpret_cast<HMENU>(id), nullptr, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
            SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
            installRoundedInputTheme(control);
            return control;
        };
        label(L"总资产", 34);
        label(L"可用资金", 82);
        context->totalAssets = edit(IDC_FUNDS_TOTAL_ASSETS, 34, context->state->accountFunds.totalAssets);
        context->availableCash = edit(IDC_FUNDS_AVAILABLE_CASH, 82, context->state->accountFunds.availableCash);
        HWND save = CreateWindowW(L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  218, 136, 82, 34, window,
                                  reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        HWND cancel = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    308, 136, 82, 34, window,
                                    reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        SendMessageW(save, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        setImmersiveDarkTitleBar(window);
        return 0;
    }
    FundsDialogContext* context = fundsContext(window);
    switch (message) {
    case WM_ERASEBKGND:
        if (context) {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, context->windowBrush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (context) {
            SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
            SetTextColor(reinterpret_cast<HDC>(wParam), themePalette().text);
            return reinterpret_cast<LRESULT>(context->windowBrush);
        }
        break;
    case WM_CTLCOLOREDIT:
        if (context) {
            SetBkColor(reinterpret_cast<HDC>(wParam), themePalette().surface);
            SetTextColor(reinterpret_cast<HDC>(wParam), themePalette().text);
            return reinterpret_cast<LRESULT>(context->surfaceBrush);
        }
        break;
    case WM_DRAWITEM:
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            drawModalButton(item, item.CtlID == IDOK);
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (!context) break;
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            auto total = numberValue(context->totalAssets);
            auto available = numberValue(context->availableCash);
            if (!total || !available || *total < 0.0 || *available < 0.0 || *available > *total) {
                MessageBoxW(window, L"总资产和可用资金必须为非负数，且可用资金不能超过总资产。",
                            L"输入错误", MB_ICONWARNING);
                return 0;
            }
            context->state->accountFunds.totalAssets = *total;
            context->state->accountFunds.availableCash = *available;
            context->state->accountFunds.updatedAt = wideToUtf8(currentMinuteText());
            context->state->accountFundsStore.save(context->state->accountFunds);
            populateHoldings(*context->state);
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void editAccountFunds(AppState& state) {
    FundsDialogContext context;
    context.state = &state;
    context.font = state.font;
    ThemePalette palette = themePalette();
    context.windowBrush = CreateSolidBrush(palette.window);
    context.surfaceBrush = CreateSolidBrush(palette.surface);
    RECT owner{};
    GetWindowRect(state.window, &owner);
    constexpr int width = 440;
    constexpr int height = 230;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AShareFundsDialog", L"资金账户",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  owner.left + (owner.right - owner.left - width) / 2,
                                  owner.top + (owner.bottom - owner.top - height) / 2,
                                  width, height, state.window, nullptr, state.instance, &context);
    if (!dialog) {
        DeleteObject(context.windowBrush);
        DeleteObject(context.surfaceBrush);
        throw std::runtime_error("无法打开资金账户窗口。");
    }
    runModalWindow(state.window, dialog);
    DeleteObject(context.windowBrush);
    DeleteObject(context.surfaceBrush);
}

}  // namespace ashare
