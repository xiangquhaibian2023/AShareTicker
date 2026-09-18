#pragma once

#include "domain.h"
#include "strategy_simulator.h"

#include <windows.h>

#include <map>
#include <string>

namespace ashare {

// 算法研究专用的期初账户参数，不修改持仓管理页面中的真实持仓。
struct ResearchSimulationSettings {
    std::map<std::string, Holding> openingHoldings;
    double initialCash = 100000.0;
    // fees 是新标的的默认值，已有标的优先使用 feesBySymbol 中的独立模板。
    SimulationFeeSchedule fees = eastmoneyBasicFeeSchedule();
    std::map<std::string, SimulationFeeSchedule> feesBySymbol;
};

LRESULT CALLBACK ResearchSettingsDialogProc(HWND window, UINT message,
                                            WPARAM wParam, LPARAM lParam);

// 返回 true 表示用户保存了参数，取消时 settings 保持不变。
bool editResearchSimulationSettings(
    HWND owner, HINSTANCE instance, HFONT font,
    const std::map<std::string, std::string>& symbols,
    ResearchSimulationSettings& settings);

}  // namespace ashare
