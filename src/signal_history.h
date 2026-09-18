#pragma once

#include "domain.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ashare {

// 一次首次出现或方向发生变化的交易信号，用于策略页累计展示。
struct StrategySignalRecord {
    MarketDataMode dataMode = MarketDataMode::Live;
    std::string strategyId;
    TradingSignal signal;
};

class StrategySignalHistory {
public:
    // 连续扫描的同方向信号只记录一次；信号消失后再次出现会形成新记录。
    bool observe(const std::string& strategyId, MarketDataMode dataMode,
                 const std::vector<TradingSignal>& signals);
    // 从当天持久化记录恢复列表和最后信号方向，避免重启后重复记录持续信号。
    void restore(std::vector<StrategySignalRecord> records);
    void reset();
    const std::vector<StrategySignalRecord>& records() const;

private:
    static constexpr std::size_t maxRecords = 500;
    std::map<std::string, SignalType> states_;
    std::vector<StrategySignalRecord> records_;
};

}  // namespace ashare
