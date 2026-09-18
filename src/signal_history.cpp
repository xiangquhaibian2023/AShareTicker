#include "signal_history.h"

#include <algorithm>
#include <cstddef>

namespace ashare {

bool StrategySignalHistory::observe(const std::string& strategyId, MarketDataMode dataMode,
                                    const std::vector<TradingSignal>& signals) {
    bool changed = false;
    for (const auto& signal : signals) {
        std::string key = strategyId + "|" + signal.symbol;
        auto previous = states_.find(key);
        SignalType previousType = previous == states_.end() ? SignalType::None : previous->second;
        if (signal.type != SignalType::None && signal.type != previousType) {
            records_.push_back(StrategySignalRecord{dataMode, strategyId, signal});
            changed = true;
        }
        states_[std::move(key)] = signal.type;
    }
    if (records_.size() > maxRecords) {
        records_.erase(records_.begin(),
                       records_.begin() + static_cast<std::ptrdiff_t>(records_.size() - maxRecords));
    }
    return changed;
}

void StrategySignalHistory::restore(std::vector<StrategySignalRecord> records) {
    reset();
    if (records.size() > maxRecords) {
        records.erase(records.begin(),
                      records.begin() + static_cast<std::ptrdiff_t>(records.size() - maxRecords));
    }
    for (const auto& record : records) {
        if (record.signal.type == SignalType::None || record.strategyId.empty() ||
            record.signal.symbol.empty()) {
            continue;
        }
        states_[record.strategyId + "|" + record.signal.symbol] = record.signal.type;
        records_.push_back(record);
    }
}

void StrategySignalHistory::reset() {
    states_.clear();
    records_.clear();
}

const std::vector<StrategySignalRecord>& StrategySignalHistory::records() const {
    return records_;
}

}  // namespace ashare
