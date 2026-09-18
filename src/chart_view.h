#pragma once

#include <windows.h>
#include <commctrl.h>

namespace ashare {

// 自绘分钟折线/日 K 子窗口过程，内部使用内存 DC 双缓冲避免刷新闪烁。
LRESULT CALLBACK KLineProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
// ListView 表头子类过程，用于保持深色主题和字体一致。
LRESULT CALLBACK HeaderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                            UINT_PTR subclassId, DWORD_PTR referenceData);

}  // namespace ashare
