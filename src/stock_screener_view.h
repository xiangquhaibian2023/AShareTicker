#pragma once

#include "app_state.h"

#include <windows.h>

namespace ashare {

// T0 策略选股独立页面；后台加载真实行情，UI 线程只负责展示和持久化操作。
LRESULT CALLBACK StockScreenerViewProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);

}  // namespace ashare
