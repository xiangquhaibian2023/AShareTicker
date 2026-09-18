#pragma once

#include "app_state.h"

#include <windows.h>

namespace ashare {

// 主窗口消息入口，负责控件创建、布局、绘制和命令分发。
LRESULT CALLBACK MainProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
// 仅在文本变化时写控件，减少定时刷新造成的界面抖动。
void setWindowTextIfChanged(HWND window, const std::wstring& text);
void setStatus(AppState& state, const std::wstring& text);
void layout(AppState& state, int width, int height);

}  // namespace ashare
