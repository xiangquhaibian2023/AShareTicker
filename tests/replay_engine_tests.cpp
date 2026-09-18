#include "replay_engine.h"
#include "quote_provider.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

ashare::KLine line(const std::string& date, double open, double close,
                   double high, double low, double volume) {
    return ashare::KLine{date, open, close, high, low, volume};
}

}  // namespace

int main() {
    using namespace ashare;

    ReplayEngine replay(std::filesystem::temp_directory_path() / "ashare_replay_test_cache");
    replay.begin("2026-08-07");
    std::vector<KLine> minute = {
        line("2026-08-07 09:30", 10.00, 10.02, 10.03, 9.99, 100.0),
        line("2026-08-07 09:31", 10.02, 10.04, 10.05, 10.01, 120.0),
        line("2026-08-07 13:00", 10.04, 10.08, 10.09, 10.03, 150.0),
    };
    // 当天完整日线故意包含未来收盘价 10.50，回放必须忽略并用分钟线聚合。
    std::vector<KLine> daily = {
        line("2026-08-06", 9.80, 10.00, 10.10, 9.70, 5000.0),
        line("2026-08-07", 10.00, 10.50, 10.60, 9.90, 8000.0),
    };
    replay.installSymbolData("sh600000", "浦发银行", minute, daily);

    assert(replay.ready());
    assert(replay.cursor() == 0);
    assert(replay.currentTime() == "09:30");
    assert(replay.minuteLines("sh600000").size() == 1);
    auto quote = replay.quote("sh600000");
    assert(quote && quote->price == 10.02 && quote->previousClose == 10.00);
    auto partialDaily = replay.dailyLines("sh600000");
    assert(partialDaily.size() == 2);
    assert(partialDaily.back().date == "2026-08-07");
    assert(partialDaily.back().close == 10.02);

    replay.advance(120);
    assert(replay.cursor() == 120);
    assert(replay.currentTime() == "11:30");
    assert(replay.minuteLines("sh600000").size() == 2);
    replay.advance();
    assert(replay.cursor() == 121);
    assert(replay.currentTime() == "13:00");
    assert(replay.minuteLines("sh600000").size() == 3);
    assert(replay.dailyLines("sh600000").back().close == 10.08);

    replay.reset();
    assert(replay.minuteLines("sh600000").size() == 1);
    assert(replay.quote("sh600000")->price == 10.02);

    // 行情从 09:31 开始时，重置应对齐首根真实分钟，不能停在空白的 09:30。
    replay.begin("2026-08-07");
    replay.installSymbolData(
        "sh600000", "浦发银行",
        {line("2026-08-07 09:31", 10.00, 10.02, 10.03, 9.99, 100.0)},
        {line("2026-08-06", 9.80, 10.00, 10.10, 9.70, 5000.0)});
    replay.reset();
    assert(replay.cursor() == 1);
    assert(replay.currentTime() == "09:31");
    assert(replay.quote("sh600000")->price == 10.02);

    bool rejectedMissingPriorClose = false;
    try {
        replay.begin("2026-08-08");
        replay.installSymbolData("sh600000", "浦发银行",
                                 {line("2026-08-08 09:30", 10.0, 10.0, 10.0, 10.0, 1.0)},
                                 {line("2026-08-08", 10.0, 10.0, 10.0, 10.0, 1.0)});
    } catch (const std::runtime_error&) {
        rejectedMissingPriorClose = true;
    }
    assert(rejectedMissingPriorClose);

    bool rejectedMissingOpening = false;
    try {
        replay.begin("2026-08-07");
        replay.installSymbolData("sh600000", "浦发银行",
                                 {line("2026-08-07 09:55", 10.0, 10.0, 10.0, 10.0, 1.0)},
                                 {line("2026-08-06", 9.9, 9.9, 9.9, 9.9, 1.0)});
    } catch (const std::runtime_error&) {
        rejectedMissingOpening = true;
    }
    assert(rejectedMissingOpening);

    // 分钟尾部增量应覆盖当前分钟、追加新分钟，并在交易日切换时自动清空旧数据。
    std::vector<KLine> cachedMinute = {
        line("2026-08-07 09:30", 10.00, 10.01, 10.02, 9.99, 100.0),
        line("2026-08-07 09:31", 10.01, 10.02, 10.03, 10.00, 120.0),
    };
    mergeIncrementalMinuteLines(
        cachedMinute,
        {line("2026-08-07 09:31", 10.01, 10.04, 10.05, 10.00, 180.0),
         line("2026-08-07 09:32", 10.04, 10.06, 10.07, 10.03, 90.0)},
        242);
    assert(cachedMinute.size() == 3);
    assert(cachedMinute[1].close == 10.04 && cachedMinute[1].volume == 180.0);
    assert(cachedMinute.back().date == "2026-08-07 09:32");
    mergeIncrementalMinuteLines(
        cachedMinute,
        {line("2026-08-10 09:30", 10.10, 10.12, 10.13, 10.09, 100.0)},
        242);
    assert(cachedMinute.size() == 1);
    assert(cachedMinute.front().date == "2026-08-10 09:30");
    mergeIncrementalMinuteLines(
        cachedMinute,
        {line("2026-08-07 15:00", 9.90, 9.91, 9.92, 9.89, 10.0)},
        242);
    assert(cachedMinute.size() == 1 &&
           cachedMinute.front().date == "2026-08-10 09:30");

    std::cout << "Replay engine tests passed\n";
    return 0;
}
