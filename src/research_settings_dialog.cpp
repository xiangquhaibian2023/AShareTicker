#include "research_settings_dialog.h"

#include "resource_ids.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ashare {
namespace {

struct ResearchSettingsContext {
    HFONT font = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH inputBrush = nullptr;
    std::map<std::string, std::string> symbols;
    std::vector<std::string> symbolOrder;
    ResearchSimulationSettings working;
    bool saved = false;
    bool loading = false;
    int loadedSymbol = -1;
    HWND symbol = nullptr;
    HWND quantity = nullptr;
    HWND cost = nullptr;
    HWND cash = nullptr;
    HWND feeTemplate = nullptr;
    HWND commission = nullptr;
    HWND minimum = nullptr;
    HWND stamp = nullptr;
    HWND transfer = nullptr;
    HWND slippage = nullptr;
};

ResearchSettingsContext* dialogContext(HWND window) {
    return reinterpret_cast<ResearchSettingsContext*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::wstring controlText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

std::optional<double> numberValue(HWND control) {
    try {
        std::wstring text = controlText(control);
        size_t end = 0;
        double value = std::stod(text, &end);
        return end == text.size() && std::isfinite(value)
                   ? std::optional<double>(value)
                   : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<long long> integerValue(HWND control) {
    try {
        std::wstring text = controlText(control);
        size_t end = 0;
        long long value = std::stoll(text, &end);
        return end == text.size() ? std::optional<long long>(value)
                                  : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void setText(HWND control, const std::wstring& value) {
    SetWindowTextW(control, value.c_str());
}

SimulationFeeSchedule& loadedFeeSchedule(ResearchSettingsContext& context) {
    if (context.loadedSymbol < 0 ||
        context.loadedSymbol >= static_cast<int>(context.symbolOrder.size())) {
        return context.working.fees;
    }
    const std::string& symbol = context.symbolOrder[
        static_cast<size_t>(context.loadedSymbol)];
    auto [found, inserted] = context.working.feesBySymbol.emplace(
        symbol, context.working.fees);
    (void)inserted;
    return found->second;
}

void loadFeeSchedule(ResearchSettingsContext& context) {
    const SimulationFeeSchedule& fees = loadedFeeSchedule(context);
    context.loading = true;
    setText(context.commission,
            formatNumber(fees.commissionRate * 10000.0, 3));
    setText(context.minimum,
            formatNumber(fees.minimumCommission, 2));
    setText(context.stamp,
            formatNumber(fees.stockStampDutyRate * 10000.0, 3));
    setText(context.transfer,
            formatNumber(fees.stockTransferFeeRate * 10000.0, 3));
    setText(context.slippage,
            formatNumber(fees.slippageRatePerSide * 10000.0, 3));
    int selection = fees.id == "eastmoney_basic"
                        ? 0
                        : fees.id == "program_default" ? 1 : 2;
    SendMessageW(context.feeTemplate, CB_SETCURSEL, selection, 0);
    context.loading = false;
}

bool saveLoadedHolding(ResearchSettingsContext& context, HWND window,
                       bool showMessage) {
    if (context.loadedSymbol < 0 ||
        context.loadedSymbol >= static_cast<int>(context.symbolOrder.size())) {
        return true;
    }
    auto quantity = integerValue(context.quantity);
    auto cost = numberValue(context.cost);
    if (!quantity || !cost || *quantity < 0 || *cost < 0.0 ||
        (*quantity > 0 && *cost <= 0.0)) {
        if (showMessage) {
            MessageBoxW(window,
                        L"期初持仓数量必须为非负整数；数量大于 0 时成本价必须大于 0。",
                        L"输入错误", MB_OK | MB_ICONWARNING);
        }
        return false;
    }
    const std::string& symbol = context.symbolOrder[
        static_cast<size_t>(context.loadedSymbol)];
    Holding& holding = context.working.openingHoldings[symbol];
    holding.symbol = symbol;
    holding.quantity = *quantity;
    holding.cost = *cost;
    clearIntradayTrades(holding);
    return true;
}

void loadSelectedHolding(ResearchSettingsContext& context, int selection) {
    if (selection < 0 || selection >= static_cast<int>(context.symbolOrder.size())) {
        return;
    }
    context.loadedSymbol = selection;
    const std::string& symbol = context.symbolOrder[static_cast<size_t>(selection)];
    auto found = context.working.openingHoldings.find(symbol);
    long long quantity = found == context.working.openingHoldings.end()
                             ? 0
                             : found->second.quantity;
    double cost = found == context.working.openingHoldings.end()
                      ? 0.0
                      : found->second.cost;
    context.loading = true;
    setText(context.quantity, std::to_wstring(quantity));
    setText(context.cost, formatNumber(cost, 3));
    context.loading = false;
}

bool saveLoadedFeeSchedule(ResearchSettingsContext& context, HWND window,
                           bool showMessage) {
    auto commission = numberValue(context.commission);
    auto minimum = numberValue(context.minimum);
    auto stamp = numberValue(context.stamp);
    auto transfer = numberValue(context.transfer);
    auto slippage = numberValue(context.slippage);
    auto validRate = [](const std::optional<double>& value) {
        return value && *value >= 0.0 && *value <= 100.0;
    };
    if (!validRate(commission) || !minimum ||
        *minimum < 0.0 || !validRate(stamp) || !validRate(transfer) ||
        !validRate(slippage)) {
        if (showMessage) {
            MessageBoxW(window,
                        L"最低佣金必须为非负数；万分费率必须在 0 到 100 之间。",
                        L"输入错误", MB_OK | MB_ICONWARNING);
        }
        return false;
    }
    SimulationFeeSchedule schedule;
    schedule.commissionRate = *commission / 10000.0;
    schedule.minimumCommission = *minimum;
    schedule.stockStampDutyRate = *stamp / 10000.0;
    schedule.stockTransferFeeRate = *transfer / 10000.0;
    schedule.slippageRatePerSide = *slippage / 10000.0;
    int templateSelection = static_cast<int>(
        SendMessageW(context.feeTemplate, CB_GETCURSEL, 0, 0));
    if (templateSelection == 0) {
        schedule.id = "eastmoney_basic";
        schedule.name = "东方财富基础账户（万 2.5）";
    } else if (templateSelection == 1) {
        schedule.id = "program_default";
        schedule.name = "程序默认（万 1）";
    } else {
        schedule.id = "custom";
        schedule.name = "自定义佣金";
    }
    loadedFeeSchedule(context) = std::move(schedule);
    return true;
}

bool readCashSetting(ResearchSettingsContext& context, HWND window) {
    auto cash = numberValue(context.cash);
    if (!cash || *cash < 0.0) {
        MessageBoxW(window, L"账户期初资金必须是非负数。",
                    L"输入错误", MB_OK | MB_ICONWARNING);
        return false;
    }
    context.working.initialCash = *cash;
    return true;
}

HWND createRoundedEdit(HWND parent, HFONT font, int id, int x, int y,
                       int width, const std::wstring& value,
                       DWORD extraStyle = 0) {
    HWND control = CreateWindowExW(
        0, L"EDIT", value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER |
            ES_MULTILINE | extraStyle,
        x, y, width, 36, parent, reinterpret_cast<HMENU>(id), nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(8, 8));
    SendMessageW(control, EM_SETLIMITTEXT, 32, 0);
    installRoundedInputTheme(control);
    return control;
}

HWND createRoundedCombo(HWND parent, HFONT font, int id, int x, int y,
                        int width) {
    HWND control = CreateWindowExW(
        0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
            CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        x, y, width, 220, parent, reinterpret_cast<HMENU>(id), nullptr, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    installRoundedComboTheme(control);
    return control;
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

LRESULT CALLBACK ResearchSettingsDialogProc(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* context = reinterpret_cast<ResearchSettingsContext*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));

        auto label = [&](const wchar_t* text, int x, int y, int width) {
            HWND control = CreateWindowW(L"STATIC", text,
                                         WS_CHILD | WS_VISIBLE | SS_CENTER,
                                         x, y, width, 28, window, nullptr, nullptr, nullptr);
            SendMessageW(control, WM_SETFONT,
                         reinterpret_cast<WPARAM>(context->font), TRUE);
        };
        HWND heading = CreateWindowW(
            L"STATIC", L"期初账户与交易费用",
            WS_CHILD | WS_VISIBLE, 28, 18, 680, 30,
            window, nullptr, nullptr, nullptr);
        SendMessageW(heading, WM_SETFONT,
                     reinterpret_cast<WPARAM>(context->font), TRUE);

        label(L"证券标的", 28, 62, 112);
        context->symbol = createRoundedCombo(
            window, context->font, IDC_RESEARCH_SETTING_SYMBOL, 150, 56, 558);
        for (const auto& [symbol, name] : context->symbols) {
            context->symbolOrder.push_back(symbol);
            std::wstring value = utf8ToWide(symbol);
            if (!name.empty() && name != symbol) value += L"  " + utf8ToWide(name);
            SendMessageW(context->symbol, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(value.c_str()));
        }

        label(L"期初持仓数量", 28, 112, 112);
        context->quantity = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_QUANTITY,
            150, 106, 190, L"0", ES_NUMBER);
        label(L"期初成本价", 374, 112, 112);
        context->cost = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_COST,
            496, 106, 212, L"0.000");

        label(L"账户期初资金", 28, 162, 112);
        context->cash = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_CASH,
            150, 156, 190, formatNumber(context->working.initialCash, 2));
        label(L"标的佣金模板", 374, 162, 112);
        context->feeTemplate = createRoundedCombo(
            window, context->font, IDC_RESEARCH_SETTING_TEMPLATE,
            496, 156, 212);
        const wchar_t* templates[] = {
            L"东方财富基础账户", L"程序默认（万 1）", L"自定义佣金"};
        for (const auto* value : templates) {
            SendMessageW(context->feeTemplate, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(value));
        }

        const int fieldX[] = {28, 170, 312, 454, 596};
        const wchar_t* fieldLabels[] = {
            L"佣金（万分）", L"最低佣金（元）", L"印花税（万分）",
            L"过户费（万分）", L"单边滑点（万分）"};
        for (int index = 0; index < 5; ++index) {
            label(fieldLabels[index], fieldX[index], 214, 128);
        }
        context->commission = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_COMMISSION,
            fieldX[0], 242, 128, L"");
        context->minimum = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_MINIMUM,
            fieldX[1], 242, 128, L"");
        context->stamp = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_STAMP,
            fieldX[2], 242, 128, L"");
        context->transfer = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_TRANSFER,
            fieldX[3], 242, 128, L"");
        context->slippage = createRoundedEdit(
            window, context->font, IDC_RESEARCH_SETTING_SLIPPAGE,
            fieldX[4], 242, 128, L"");

        HWND hint = CreateWindowW(
            L"STATIC",
            L"切换证券时可分别设置佣金模板；账户资金按可回测标的等额分配，场内基金不收印花税和过户费。",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            28, 300, 696, 28, window, nullptr, nullptr, nullptr);
        SendMessageW(hint, WM_SETFONT,
                     reinterpret_cast<WPARAM>(context->font), TRUE);

        HWND cancel = CreateWindowW(
            L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            532, 350, 84, 36, window, reinterpret_cast<HMENU>(IDCANCEL), nullptr, nullptr);
        HWND save = CreateWindowW(
            L"BUTTON", L"保存参数", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            626, 350, 98, 36, window, reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        SendMessageW(save, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);

        if (!context->symbolOrder.empty()) {
            SendMessageW(context->symbol, CB_SETCURSEL, 0, 0);
            loadSelectedHolding(*context, 0);
            loadFeeSchedule(*context);
        } else {
            loadFeeSchedule(*context);
        }
        setImmersiveDarkTitleBar(window);
        return 0;
    }

    auto* context = dialogContext(window);
    switch (message) {
    case WM_ERASEBKGND:
        if (context) {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, context->windowBrush);
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
        if (context) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_BUTTON) {
                drawModalButton(item, item.CtlID == IDOK);
                return TRUE;
            }
            if (item.CtlType == ODT_COMBOBOX) {
                return drawThemedComboItem(item);
            }
        }
        break;
    case WM_COMMAND:
        if (!context) break;
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_SETTING_SYMBOL &&
            HIWORD(wParam) == CBN_SELCHANGE) {
            int previous = context->loadedSymbol;
            int selection = static_cast<int>(
                SendMessageW(context->symbol, CB_GETCURSEL, 0, 0));
            if (!saveLoadedHolding(*context, window, true) ||
                !saveLoadedFeeSchedule(*context, window, true)) {
                SendMessageW(context->symbol, CB_SETCURSEL, previous, 0);
                return 0;
            }
            loadSelectedHolding(*context, selection);
            loadFeeSchedule(*context);
            return 0;
        }
        if (LOWORD(wParam) == IDC_RESEARCH_SETTING_TEMPLATE &&
            HIWORD(wParam) == CBN_SELCHANGE && !context->loading) {
            int selection = static_cast<int>(
                SendMessageW(context->feeTemplate, CB_GETCURSEL, 0, 0));
            if (selection == 0) {
                loadedFeeSchedule(*context) = eastmoneyBasicFeeSchedule();
                loadFeeSchedule(*context);
            } else if (selection == 1) {
                loadedFeeSchedule(*context) = SimulationFeeSchedule{};
                loadFeeSchedule(*context);
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_CHANGE && !context->loading &&
            LOWORD(wParam) >= IDC_RESEARCH_SETTING_COMMISSION &&
            LOWORD(wParam) <= IDC_RESEARCH_SETTING_SLIPPAGE) {
            SendMessageW(context->feeTemplate, CB_SETCURSEL, 2, 0);
            return 0;
        }
        if (LOWORD(wParam) == IDOK) {
            if (!saveLoadedHolding(*context, window, true) ||
                !saveLoadedFeeSchedule(*context, window, true) ||
                !readCashSetting(*context, window)) {
                return 0;
            }
            context->saved = true;
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

bool editResearchSimulationSettings(
    HWND owner, HINSTANCE instance, HFONT font,
    const std::map<std::string, std::string>& symbols,
    ResearchSimulationSettings& settings) {
    if (symbols.empty()) {
        MessageBoxW(owner, L"当前标的范围为空，无法设置回测参数。",
                    L"算法研究", MB_OK | MB_ICONINFORMATION);
        return false;
    }
    ResearchSettingsContext context;
    context.font = font;
    context.symbols = symbols;
    context.working = settings;
    for (const auto& [symbol, name] : symbols) {
        (void)name;
        context.working.feesBySymbol.emplace(symbol, context.working.fees);
    }
    ThemePalette palette = themePalette();
    context.windowBrush = CreateSolidBrush(palette.window);
    context.inputBrush = CreateSolidBrush(palette.surfaceAlt);

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    constexpr int width = 760;
    constexpr int height = 445;
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"AShareResearchSettingsDialog", L"算法研究参数",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2,
        ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2,
        width, height, owner, nullptr, instance, &context);
    if (!dialog) {
        DeleteObject(context.windowBrush);
        DeleteObject(context.inputBrush);
        throw std::runtime_error("无法打开算法研究参数窗口。");
    }
    runModalWindow(owner, dialog);
    DeleteObject(context.windowBrush);
    DeleteObject(context.inputBrush);
    if (context.saved) {
        settings = std::move(context.working);
        return true;
    }
    return false;
}

}  // namespace ashare
