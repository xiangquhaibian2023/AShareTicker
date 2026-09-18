#include "ui_theme.h"

#include <commctrl.h>

#include <iterator>
#include <string>

namespace ashare {

namespace {

constexpr UINT_PTR roundedInputSubclassId = 1;
constexpr UINT_PTR roundedComboSubclassId = 2;
constexpr int controlCornerRadius = 8;

void applyRoundedRegion(HWND control) {
    RECT rect{};
    GetClientRect(control, &rect);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    HRGN region = CreateRoundRectRgn(rect.left, rect.top, rect.right + 1, rect.bottom + 1,
                                     controlCornerRadius, controlCornerRadius);
    if (!SetWindowRgn(control, region, TRUE)) {
        DeleteObject(region);
    }
}

void centerInputText(HWND control) {
    RECT client{};
    GetClientRect(control, &client);
    if (client.right <= client.left || client.bottom <= client.top) {
        return;
    }
    HDC dc = GetDC(control);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HFONT previousFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    if (previousFont) {
        SelectObject(dc, previousFont);
    }
    ReleaseDC(control, dc);

    int clientWidth = static_cast<int>(client.right - client.left);
    int clientHeight = static_cast<int>(client.bottom - client.top);
    int textHeight = std::max(1, static_cast<int>(metrics.tmHeight));
    int top = std::max(2, (clientHeight - textHeight) / 2);
    RECT formatting{8, top, std::max(9, clientWidth - 8),
                    std::min(clientHeight - 2, top + textHeight + 2)};
    SendMessageW(control, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
}

void drawRoundedInputBorder(HWND control) {
    HDC dc = GetWindowDC(control);
    if (!dc) {
        return;
    }
    RECT windowRect{};
    GetWindowRect(control, &windowRect);
    OffsetRect(&windowRect, -windowRect.left, -windowRect.top);
    ThemePalette palette = themePalette();
    COLORREF borderColor = GetFocus() == control ? palette.accent : palette.border;
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ previousPen = SelectObject(dc, pen);
    HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(dc, windowRect.left, windowRect.top, windowRect.right,
              windowRect.bottom, controlCornerRadius, controlCornerRadius);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(pen);
    ReleaseDC(control, dc);
}

LRESULT CALLBACK RoundedInputProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_SIZE: {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        applyRoundedRegion(control);
        centerInputText(control);
        return result;
    }
    case WM_SETFONT: {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        centerInputText(control);
        return result;
    }
    case WM_CHAR:
        if (wParam == L'\r' || wParam == L'\n') {
            return 0;
        }
        break;
    case WM_PAINT: {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        drawRoundedInputBorder(control);
        return result;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        RedrawWindow(control, nullptr, nullptr,
                     RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(control, RoundedInputProc, roundedInputSubclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

void paintRoundedCombo(HWND control, HDC dc) {
    RECT rect{};
    GetClientRect(control, &rect);
    ThemePalette palette = themePalette();
    bool focused = GetFocus() == control;
    bool dropped = SendMessageW(control, CB_GETDROPPEDSTATE, 0, 0) != 0;
    bool enabled = IsWindowEnabled(control) != FALSE;

    HBRUSH brush = CreateSolidBrush(palette.surfaceAlt);
    HPEN pen = CreatePen(PS_SOLID, 1, focused || dropped ? palette.accent : palette.border);
    HGDIOBJ previousBrush = SelectObject(dc, brush);
    HGDIOBJ previousPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
              controlCornerRadius, controlCornerRadius);
    SelectObject(dc, previousBrush);
    SelectObject(dc, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);

    std::wstring selectedText;
    LRESULT selection = SendMessageW(control, CB_GETCURSEL, 0, 0);
    if (selection != CB_ERR) {
        LRESULT length = SendMessageW(control, CB_GETLBTEXTLEN, selection, 0);
        if (length >= 0) {
            selectedText.resize(static_cast<size_t>(length) + 1);
            SendMessageW(control, CB_GETLBTEXT, selection,
                         reinterpret_cast<LPARAM>(selectedText.data()));
            selectedText.resize(static_cast<size_t>(length));
        }
    }

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? palette.text : palette.muted);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0));
    HFONT previousFont = font ? static_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    RECT textRect{rect.left + 10, rect.top, rect.right - 30, rect.bottom};
    DrawTextW(dc, selectedText.c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    COLORREF arrowColor = focused || dropped ? palette.accent : palette.muted;
    HPEN arrowPen = CreatePen(PS_SOLID, 2, arrowColor);
    previousPen = SelectObject(dc, arrowPen);
    int centerX = rect.right - 16;
    int centerY = (rect.top + rect.bottom) / 2;
    MoveToEx(dc, centerX - 4, centerY - 2, nullptr);
    LineTo(dc, centerX, centerY + 2);
    LineTo(dc, centerX + 5, centerY - 3);
    SelectObject(dc, previousPen);
    DeleteObject(arrowPen);
    if (previousFont) {
        SelectObject(dc, previousFont);
    }
}

LRESULT CALLBACK RoundedComboProc(HWND control, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_SIZE: {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        applyRoundedRegion(control);
        return result;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(control, &paint);
        paintRoundedCombo(control, dc);
        EndPaint(control, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paintRoundedCombo(control, reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case CB_SHOWDROPDOWN: {
        LRESULT result = DefSubclassProc(control, message, wParam, lParam);
        InvalidateRect(control, nullptr, TRUE);
        return result;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(control, RoundedComboProc, roundedComboSubclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(control, message, wParam, lParam);
}

}  // namespace

ThemePalette themePalette() {
    // 深海军蓝工作台配合金色主操作；涨跌使用柔和粉红和青绿，便于长时间阅读。
    return {
        RGB(5, 18, 31), RGB(8, 27, 45), RGB(11, 32, 51), RGB(6, 20, 34),
        RGB(28, 49, 68), RGB(236, 242, 248), RGB(119, 137, 156),
        RGB(218, 171, 31), RGB(8, 20, 31), RGB(18, 44, 66), RGB(34, 54, 73),
        RGB(244, 159, 190), RGB(72, 190, 171),
    };
}

void setImmersiveDarkTitleBar(HWND window) {
    // 新旧 Windows 10 对深色标题栏使用过不同属性编号，因此依次尝试。
    HMODULE module = LoadLibraryW(L"dwmapi.dll");
    if (!module) return;
    using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setAttribute = reinterpret_cast<SetAttribute>(GetProcAddress(module, "DwmSetWindowAttribute"));
    if (setAttribute) {
        BOOL enabled = TRUE;
        constexpr DWORD immersiveDarkMode = 20;
        constexpr DWORD immersiveDarkModeLegacy = 19;
        if (FAILED(setAttribute(window, immersiveDarkMode, &enabled, sizeof(enabled)))) {
            setAttribute(window, immersiveDarkModeLegacy, &enabled, sizeof(enabled));
        }
    }
    FreeLibrary(module);
}

void setExplorerControlTheme(HWND control) {
    HMODULE module = LoadLibraryW(L"uxtheme.dll");
    if (!module) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    auto setWindowTheme = reinterpret_cast<SetWindowThemeFn>(GetProcAddress(module, "SetWindowTheme"));
    if (setWindowTheme) setWindowTheme(control, L"DarkMode_Explorer", nullptr);
    FreeLibrary(module);
}

void installRoundedInputTheme(HWND control) {
    if (!control) {
        return;
    }
    SetWindowLongPtrW(control, GWL_EXSTYLE,
                      GetWindowLongPtrW(control, GWL_EXSTYLE) & ~WS_EX_STATICEDGE);
    setExplorerControlTheme(control);
    SetWindowSubclass(control, RoundedInputProc, roundedInputSubclassId, 0);
    applyRoundedRegion(control);
    centerInputText(control);
}

void installRoundedComboTheme(HWND control) {
    if (!control) {
        return;
    }
    SetWindowSubclass(control, RoundedComboProc, roundedComboSubclassId, 0);
    applyRoundedRegion(control);
    SendMessageW(control, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), 32);
    SendMessageW(control, CB_SETITEMHEIGHT, 0, 28);
}

LRESULT drawThemedComboItem(const DRAWITEMSTRUCT& item) {
    ThemePalette palette = themePalette();
    bool selected = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH brush = CreateSolidBrush(selected ? palette.selection : palette.surfaceAlt);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);

    if (item.itemID == static_cast<UINT>(-1)) {
        return TRUE;
    }
    LRESULT length = SendMessageW(item.hwndItem, CB_GETLBTEXTLEN, item.itemID, 0);
    std::wstring value;
    if (length >= 0) {
        value.resize(static_cast<size_t>(length) + 1);
        SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID,
                     reinterpret_cast<LPARAM>(value.data()));
        value.resize(static_cast<size_t>(length));
    }
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, palette.text);
    RECT textRect = item.rcItem;
    textRect.left += 8;
    textRect.right -= 8;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    HFONT previousFont = font ? static_cast<HFONT>(SelectObject(item.hDC, font)) : nullptr;
    DrawTextW(item.hDC, value.c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (previousFont) {
        SelectObject(item.hDC, previousFont);
    }
    return TRUE;
}

void drawModalButton(const DRAWITEMSTRUCT& item, bool primary) {
    // 主操作使用强调色，普通操作维持表面色；按下时用选中背景反馈。
    ThemePalette palette = themePalette();
    RECT rect = item.rcItem;
    bool pressed = (item.itemState & ODS_SELECTED) != 0;
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF fill = primary ? palette.accent : palette.surface;
    COLORREF border = primary ? palette.accent : palette.border;
    COLORREF text = disabled ? palette.muted : primary ? palette.accentText : palette.text;
    if (pressed) fill = primary ? RGB(190, 143, 18) : palette.selection;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ previousBrush = SelectObject(item.hDC, brush);
    HGDIOBJ previousPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom,
              controlCornerRadius, controlCornerRadius);
    SelectObject(item.hDC, previousBrush);
    SelectObject(item.hDC, previousPen);
    DeleteObject(brush);
    DeleteObject(pen);
    wchar_t label[64]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(std::size(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    if (pressed) OffsetRect(&rect, 0, 1);
    DrawTextW(item.hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
}

}  // namespace ashare
