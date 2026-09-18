#pragma once

#include "app_state.h"

#include <windows.h>

namespace ashare {

// 分笔成交、资金账户和策略管理使用独立 Win32 模态窗口过程。
LRESULT CALLBACK TradeDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK FundsDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK StrategyManagerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HoldingDialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

void editHolding(AppState& state);
void editTodayTrades(AppState& state);
void editAccountFunds(AppState& state);
void manageStrategies(AppState& state);

}  // namespace ashare
