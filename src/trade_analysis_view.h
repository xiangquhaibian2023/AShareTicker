#pragma once

#include "app_state.h"

#include <windows.h>

namespace ashare {

// 券商成交分析与实时持仓优化独立页面。
LRESULT CALLBACK TradeAnalysisViewProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam);

}  // namespace ashare

