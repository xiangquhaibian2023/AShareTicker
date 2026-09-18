#pragma once

#include "app_state.h"

#include <stdexcept>
#include <string>

namespace ashare {

// 页面与业务操作控制层：组织网络、缓存、持久化和视图更新。
void switchDetailView(AppState& state, DetailView view);
void loadKLine(AppState& state, const std::string& symbol, int klt = -1);
void refreshQuoteLists(AppState& state);
void refreshDashboard(AppState& state, bool refreshChart);
void searchSymbol(AppState& state);
void addFavorite(AppState& state);
void deleteFavorite(AppState& state);
void saveHolding(AppState& state);
bool saveHoldingValues(AppState& state, const std::string& rawSymbol,
                       long long quantity, double cost, HWND messageOwner);
void deleteHolding(AppState& state);
void toggleStrategySymbol(AppState& state);
void deleteStrategySymbol(AppState& state);
// 策略扫描在后台线程执行，结果通过 WM_APP_STRATEGY_RESULT 回到 UI 线程。
void startStrategyScan(AppState& state);
void toggleTechnicalSwing(AppState& state);
void handleStrategyResult(AppState& state);
void restoreTodayStrategySignals(AppState& state);
void switchKLine(AppState& state, int klt);
// 实盘/回放模式及回放进度控制。
void selectDataMode(AppState& state);
void loadReplaySession(AppState& state);
void toggleReplayPlayback(AppState& state);
void advanceReplay(AppState& state);
void resetReplay(AppState& state);
void showError(AppState& state, const std::exception& error);
void openLogDirectory(AppState& state);

}  // namespace ashare
