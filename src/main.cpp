#include "chart_view.h"
#include "dialogs.h"
#include "main_window.h"
#include "research_view.h"
#include "research_settings_dialog.h"
#include "stock_screener_view.h"
#include "trade_analysis_view.h"

#include <windows.h>
#include <commctrl.h>

using namespace ashare;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // 禁止系统对整个窗口做位图缩放，控件布局和鼠标坐标统一使用真实像素。
    using SetDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    auto setDpiAwarenessContext = reinterpret_cast<SetDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4));
    if (!setDpiAwarenessContext || !setDpiAwarenessContext(perMonitorV2)) {
        SetProcessDPIAware();
    }
    // ListView 等通用控件在创建窗口前初始化。
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    // 分别注册图表、模态对话框和主窗口类。
    WNDCLASSW klineClass{};
    klineClass.hInstance = instance;
    klineClass.lpfnWndProc = KLineProc;
    klineClass.lpszClassName = L"AShareKLineView";
    klineClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klineClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&klineClass);

    WNDCLASSW researchClass{};
    researchClass.hInstance = instance;
    researchClass.lpfnWndProc = ResearchViewProc;
    researchClass.lpszClassName = L"AShareResearchView";
    researchClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    researchClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&researchClass);

    WNDCLASSW screenerClass{};
    screenerClass.hInstance = instance;
    screenerClass.lpfnWndProc = StockScreenerViewProc;
    screenerClass.lpszClassName = L"AShareStockScreenerView";
    screenerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    screenerClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&screenerClass);

    WNDCLASSW tradeAnalysisClass{};
    tradeAnalysisClass.hInstance = instance;
    tradeAnalysisClass.lpfnWndProc = TradeAnalysisViewProc;
    tradeAnalysisClass.lpszClassName = L"AShareTradeAnalysisView";
    tradeAnalysisClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    tradeAnalysisClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&tradeAnalysisClass);

    WNDCLASSW tradeDialogClass{};
    tradeDialogClass.hInstance = instance;
    tradeDialogClass.lpfnWndProc = TradeDialogProc;
    tradeDialogClass.lpszClassName = L"AShareTradeDialog";
    tradeDialogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    tradeDialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&tradeDialogClass);

    WNDCLASSW fundsDialogClass{};
    fundsDialogClass.hInstance = instance;
    fundsDialogClass.lpfnWndProc = FundsDialogProc;
    fundsDialogClass.lpszClassName = L"AShareFundsDialog";
    fundsDialogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    fundsDialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&fundsDialogClass);

    WNDCLASSW holdingDialogClass{};
    holdingDialogClass.hInstance = instance;
    holdingDialogClass.lpfnWndProc = HoldingDialogProc;
    holdingDialogClass.lpszClassName = L"AShareHoldingDialog";
    holdingDialogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    holdingDialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&holdingDialogClass);

    WNDCLASSW researchSettingsDialogClass{};
    researchSettingsDialogClass.hInstance = instance;
    researchSettingsDialogClass.lpfnWndProc = ResearchSettingsDialogProc;
    researchSettingsDialogClass.lpszClassName = L"AShareResearchSettingsDialog";
    researchSettingsDialogClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    researchSettingsDialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&researchSettingsDialogClass);

    WNDCLASSW strategyManagerClass{};
    strategyManagerClass.hInstance = instance;
    strategyManagerClass.lpfnWndProc = StrategyManagerProc;
    strategyManagerClass.lpszClassName = L"AShareStrategyManager";
    strategyManagerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    strategyManagerClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&strategyManagerClass);

    WNDCLASSW mainClass{};
    mainClass.hInstance = instance;
    mainClass.lpfnWndProc = MainProc;
    mainClass.lpszClassName = L"AShareTickerClientWindow";
    mainClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&mainClass);

    // WS_CLIPCHILDREN 防止父窗口重绘覆盖图表和列表子窗口。
    HWND hwnd = CreateWindowExW(0, mainClass.lpszClassName, L"A股行情客户端",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1180, 800,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        return 1;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    // 所有界面和 AppState 修改都在该 UI 消息线程中执行。
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
