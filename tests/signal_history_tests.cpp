#include "signal_history.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

ashare::TradingSignal signal(const char* symbol, ashare::SignalType type, double price) {
    ashare::TradingSignal value;
    value.symbol = symbol;
    value.algorithm = "T0 日内回转";
    value.type = type;
    value.currentPrice = price;
    value.time = "2026-08-07 10:30";
    return value;
}

}  // namespace

int main() {
    using namespace ashare;

    StrategySignalHistory history;
    assert(!history.observe("t0", MarketDataMode::Replay,
                            {signal("sh600000", SignalType::None, 10.0)}));
    assert(history.records().empty());

    assert(history.observe("t0", MarketDataMode::Replay,
                           {signal("sh600000", SignalType::Buy, 9.9)}));
    assert(history.records().size() == 1);
    // 连续同方向扫描不重复记录。
    assert(!history.observe("t0", MarketDataMode::Replay,
                            {signal("sh600000", SignalType::Buy, 9.91)}));
    assert(history.records().size() == 1);

    // 方向变化和信号消失后再次出现都必须形成新的交易点。
    assert(history.observe("t0", MarketDataMode::Replay,
                           {signal("sh600000", SignalType::Sell, 10.1)}));
    assert(history.records().size() == 2);
    history.observe("t0", MarketDataMode::Replay,
                    {signal("sh600000", SignalType::None, 10.0)});
    assert(history.observe("t0", MarketDataMode::Replay,
                           {signal("sh600000", SignalType::Sell, 10.2)}));
    assert(history.records().size() == 3);

    // 不同策略维护独立状态。
    assert(history.observe("breakout", MarketDataMode::Live,
                           {signal("sh600000", SignalType::Sell, 10.3)}));
    assert(history.records().size() == 4);
    assert(history.records().back().dataMode == MarketDataMode::Live);

    // 恢复后保留已有明细和最后方向，相同持续信号不能再次追加。
    std::vector<StrategySignalRecord> saved = history.records();
    StrategySignalHistory restored;
    restored.restore(saved);
    assert(restored.records().size() == saved.size());
    assert(!restored.observe("breakout", MarketDataMode::Live,
                             {signal("sh600000", SignalType::Sell, 10.4)}));

    history.reset();
    assert(history.records().empty());
    std::cout << "Signal history tests passed\n";
    return 0;
}
