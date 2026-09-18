#pragma once

#include "app_state.h"

#include <windows.h>

namespace ashare {

// 独立算法研究页面。页面内部负责后台批量分析、汇总曲线和明细列表。
LRESULT CALLBACK ResearchViewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

// 策略配置新增、删除或切换后，同步刷新研究页的算法选择框。
void refreshResearchConfiguration(AppState& state);

}  // namespace ashare
