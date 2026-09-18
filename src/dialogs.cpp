#include "dialogs.h"

#include "dashboard_view.h"
#include "main_window.h"
#include "market_controller.h"
#include "resource_ids.h"
#include "trading_algorithm.h"
#include "ui_theme.h"
#include "utils.h"

#include <commctrl.h>

#include <algorithm>
#include <stdexcept>

namespace ashare {

std::wstring strategyKindText(StrategyKind kind) {
    switch (kind) {
    case StrategyKind::Breakout: return L"趋势突破";
    case StrategyKind::MeanReversion: return L"均值回归";
    case StrategyKind::T0Intraday: return L"T0 日内回转";
    default: return L"均线动量";
    }
}

// 策略管理窗口直接编辑 AppState；父窗口禁用期间不会出现并发 UI 修改。
struct StrategyManagerContext {
    AppState* state = nullptr;
    HFONT font = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HWND list = nullptr;
    HWND nameEdit = nullptr;
    HWND kindCombo = nullptr;
};

StrategyManagerContext* strategyManagerContext(HWND window) {
    return reinterpret_cast<StrategyManagerContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void populateStrategyManagerList(StrategyManagerContext& context) {
    SendMessageW(context.list, LB_RESETCONTENT, 0, 0);
    for (const auto& config : context.state->strategies) {
        std::wstring row = utf8ToWide(config.name) + L"  [" + strategyKindText(config.kind) + L"]";
        SendMessageW(context.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
    }
    SendMessageW(context.list, LB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK StrategyManagerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        auto* context = reinterpret_cast<StrategyManagerContext*>(
            reinterpret_cast<LPCREATESTRUCTW>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        auto createLabel = [&](const wchar_t* text, int x, int y, int width) {
            HWND label = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE,
                                       x, y, width, 24, window, nullptr, nullptr, nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        };
        createLabel(L"已有策略", 20, 16, 220);
        createLabel(L"策略名称", 265, 52, 200);
        createLabel(L"算法模板", 265, 122, 200);
        context->list = CreateWindowExW(WS_EX_STATICEDGE, L"LISTBOX", L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                                        20, 44, 220, 260, window,
                                        reinterpret_cast<HMENU>(IDC_STRATEGY_LIST), nullptr, nullptr);
        context->nameEdit = CreateWindowExW(0, L"EDIT", L"",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                                ES_CENTER | ES_MULTILINE,
                                            265, 78, 210, 34, window,
                                            reinterpret_cast<HMENU>(IDC_STRATEGY_NAME), nullptr, nullptr);
        context->kindCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
                                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                                 CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                             265, 148, 210, 180, window,
                                             reinterpret_cast<HMENU>(IDC_STRATEGY_KIND), nullptr, nullptr);
        const wchar_t* kinds[] = {L"均线动量", L"趋势突破", L"均值回归", L"T0 日内回转"};
        for (const wchar_t* kind : kinds) {
            SendMessageW(context->kindCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kind));
        }
        SendMessageW(context->kindCombo, CB_SETCURSEL, 0, 0);
        HWND add = CreateWindowW(L"BUTTON", L"新增策略", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 265, 196, 100, 34, window,
                                 reinterpret_cast<HMENU>(IDC_STRATEGY_ADD), nullptr, nullptr);
        HWND remove = CreateWindowW(L"BUTTON", L"删除选中", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    375, 196, 100, 34, window,
                                    reinterpret_cast<HMENU>(IDC_STRATEGY_DELETE), nullptr, nullptr);
        HWND close = CreateWindowW(L"BUTTON", L"完成", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   375, 270, 100, 34, window,
                                   reinterpret_cast<HMENU>(IDOK), nullptr, nullptr);
        HWND controls[] = {context->list, context->nameEdit, context->kindCombo, add, remove, close};
        for (HWND control : controls) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->font), TRUE);
        }
        SendMessageW(context->nameEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        SendMessageW(context->nameEdit, EM_SETLIMITTEXT, 48, 0);
        setExplorerControlTheme(context->list);
        installRoundedInputTheme(context->nameEdit);
        installRoundedComboTheme(context->kindCombo);
        populateStrategyManagerList(*context);
        setImmersiveDarkTitleBar(window);
        return 0;
    }
    StrategyManagerContext* context = strategyManagerContext(window);
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
    case WM_CTLCOLORLISTBOX:
        if (context) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ThemePalette palette = themePalette();
            SetBkMode(dc, TRANSPARENT);
            SetBkColor(dc, palette.surface);
            SetTextColor(dc, palette.text);
            return reinterpret_cast<LRESULT>(message == WM_CTLCOLORSTATIC ? context->windowBrush
                                                                           : context->surfaceBrush);
        }
        break;
    case WM_CTLCOLOREDIT:
        if (context) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ThemePalette palette = themePalette();
            SetBkColor(dc, palette.surface);
            SetTextColor(dc, palette.text);
            return reinterpret_cast<LRESULT>(context->surfaceBrush);
        }
        break;
    case WM_DRAWITEM:
        if (context) {
            const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_BUTTON) {
                drawModalButton(item, item.CtlID == IDC_STRATEGY_ADD);
                return TRUE;
            }
            if (item.CtlType == ODT_COMBOBOX) {
                return drawThemedComboItem(item);
            }
        }
        break;
    case WM_COMMAND:
        if (!context) break;
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDC_STRATEGY_ADD) {
            wchar_t nameBuffer[64]{};
            GetDlgItemTextW(window, IDC_STRATEGY_NAME, nameBuffer,
                            static_cast<int>(std::size(nameBuffer)));
            std::string name = trim(wideToUtf8(nameBuffer));
            int selectedKind = static_cast<int>(SendDlgItemMessageW(window, IDC_STRATEGY_KIND,
                                                                    CB_GETCURSEL, 0, 0));
            if (name.empty()) {
                MessageBoxW(window, L"请输入策略名称。", L"提示", MB_ICONINFORMATION);
                return 0;
            }
            if (selectedKind == CB_ERR) {
                selectedKind = 0;
            }
            bool duplicate = std::any_of(context->state->strategies.begin(), context->state->strategies.end(),
                                         [&](const StrategyConfig& config) { return config.name == name; });
            if (duplicate) {
                MessageBoxW(window, L"策略名称已经存在。", L"提示", MB_ICONINFORMATION);
                return 0;
            }
            StrategyKind kind = selectedKind == 1 ? StrategyKind::Breakout :
                                selectedKind == 2 ? StrategyKind::MeanReversion :
                                selectedKind == 3 ? StrategyKind::T0Intraday :
                                                    StrategyKind::MovingAverageMomentum;
            // 显示名称允许中文，持久化 ID 使用进程启动后的毫秒数生成。
            std::string id = "custom_" + std::to_string(GetTickCount64());
            context->state->strategies.push_back(StrategyConfig{id, name, kind});
            context->state->strategyConfigStore.save(context->state->strategies);
            logger().write("INFO", "Strategy added id=" + id + " kind=" + strategyKindKey(kind));
            populateStrategyManagerList(*context);
            SendMessageW(context->list, LB_SETCURSEL,
                         static_cast<WPARAM>(context->state->strategies.size() - 1), 0);
            SetWindowTextW(context->nameEdit, L"");
            return 0;
        }
        if (LOWORD(wParam) == IDC_STRATEGY_DELETE) {
            if (context->state->strategies.size() <= 1) {
                MessageBoxW(window, L"至少需要保留一个策略。", L"提示", MB_ICONINFORMATION);
                return 0;
            }
            int selected = static_cast<int>(SendMessageW(context->list, LB_GETCURSEL, 0, 0));
            if (selected == LB_ERR || selected >= static_cast<int>(context->state->strategies.size())) {
                return 0;
            }
            // 删除当前策略时自动切到第一项，保证 activeStrategyId 始终有效。
            bool deletingActive = context->state->strategies[static_cast<size_t>(selected)].id ==
                                  context->state->activeStrategyId;
            std::string deletedId = context->state->strategies[static_cast<size_t>(selected)].id;
            context->state->strategies.erase(context->state->strategies.begin() + selected);
            if (deletingActive) {
                context->state->activeStrategyId = context->state->strategies.front().id;
                context->state->strategySignals.clear();
                context->state->strategyConfigStore.saveActive(context->state->activeStrategyId);
            }
            context->state->strategyConfigStore.save(context->state->strategies);
            logger().write("INFO", "Strategy deleted id=" + deletedId);
            populateStrategyManagerList(*context);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void manageStrategies(AppState& state) {
    // 与当日成交窗口相同，手工维护模态消息循环并在关闭后刷新主界面组合框。
    StrategyManagerContext context;
    context.state = &state;
    context.font = state.font;
    ThemePalette palette = themePalette();
    context.windowBrush = CreateSolidBrush(palette.window);
    context.surfaceBrush = CreateSolidBrush(palette.surface);
    RECT owner{};
    GetWindowRect(state.window, &owner);
    constexpr int width = 520;
    constexpr int height = 370;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"AShareStrategyManager", L"策略管理",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  owner.left + (owner.right - owner.left - width) / 2,
                                  owner.top + (owner.bottom - owner.top - height) / 2,
                                  width, height, state.window, nullptr, state.instance, &context);
    if (!dialog) {
        DeleteObject(context.windowBrush);
        DeleteObject(context.surfaceBrush);
        throw std::runtime_error("无法打开策略管理窗口。");
    }
    EnableWindow(state.window, FALSE);
    ShowWindow(dialog, SW_SHOW);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(state.window, TRUE);
    SetForegroundWindow(state.window);
    DeleteObject(context.windowBrush);
    DeleteObject(context.surfaceBrush);
    populateStrategyCombo(state);
    populateStrategySignals(state);
    if (!state.strategyScanRunning) {
        startStrategyScan(state);
    }
}

}  // namespace ashare
