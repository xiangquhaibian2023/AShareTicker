#pragma once

#include <windows.h>

namespace ashare {

// 固定深色主题所需的语义颜色，绘制代码不直接散落 RGB 常量。
struct ThemePalette {
    COLORREF window;
    COLORREF surface;
    COLORREF surfaceAlt;
    COLORREF sidebar;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF accentText;
    COLORREF selection;
    COLORREF grid;
    COLORREF up;
    COLORREF down;
};

ThemePalette themePalette();
// 同步设置系统标题栏及原生控件的深色外观。
void setImmersiveDarkTitleBar(HWND window);
void setExplorerControlTheme(HWND control);
// 输入框和下拉框使用客户端统一的圆角深色外观，避免系统主题产生白色方框。
void installRoundedInputTheme(HWND control);
void installRoundedComboTheme(HWND control);
LRESULT drawThemedComboItem(const DRAWITEMSTRUCT& item);
// 对话框中的 owner-draw 按钮统一由此绘制。
void drawModalButton(const DRAWITEMSTRUCT& item, bool primary);

}  // namespace ashare
