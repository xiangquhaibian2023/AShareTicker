#pragma once

#include "app_state.h"

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace ashare {

// 本模块只负责把 AppState 中的数据增量呈现到控件，不发起网络请求。
std::wstring getEditText(HWND edit);
std::wstring signalTypeText(SignalType type);
void populateQuotes(HWND list, const std::vector<Quote>& quotes);
void setQuoteColumns(HWND list);
std::vector<Quote> cachedQuotesInOrder(const AppState& state,
                                       const std::vector<std::string>& symbols);
void populateHoldings(AppState& state);
void populateStrategySignals(AppState& state);
void populateTechnicalSwingPanel(AppState& state);
void configureSignalHistoryColumns(AppState& state);
void populateSignalHistory(AppState& state);
void populateTradeDetails(AppState& state);
void configureDetailColumns(AppState& state);
void populateDetailView(AppState& state);
void populateStrategyCombo(AppState& state);
void selectActiveStrategyFromCombo(AppState& state);
void updateHoldingSummary(AppState& state);
// 根据当前页面显示对应工具栏控件，并隐藏无关输入项。
void updateToolbarVisibility(AppState& state);
void selectSymbolInList(HWND list, const std::string& symbol);
std::optional<std::string> selectedSymbolFromList(HWND list);

}  // namespace ashare
