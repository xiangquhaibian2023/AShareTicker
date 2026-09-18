#include "technical_swing_strategy.h"
#include "technical_indicators.h"
#include "storage.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

using namespace ashare;
namespace {
std::string date(size_t i) {
    // 合成数据使用严格升序的日期标签，与真实缓存样例分开。
    std::ostringstream out;
    out << "T" << std::setw(5) << std::setfill('0') << i;
    return out.str();
}
std::vector<KLine> waves(size_t n = 160, double drift = 0.0) {
    std::vector<KLine> bars;
    for (size_t i = 0; i < n; ++i) {
        double p = 10 + std::sin(i * 3.141592653589793 / 10) + drift * i;
        bars.push_back({date(i), p - 0.02, p, p + 0.1, p - 0.1, 1000});
    }
    return bars;
}
void append(std::vector<KLine>& bars, double open, double close, double high, double low, double volume) {
    bars.push_back({date(bars.size()), open, close, high, low, volume});
}
bool buying(const std::string& signal) {
    return signal == "EARLY_BUY" || signal == "BUY_CONFIRMATION" || signal == "STRONG_BUY_CONFIRMATION";
}
void equivalent(const TechnicalSwingResult& a, const TechnicalSwingResult& b) {
    assert(a.technicalSignal == b.technicalSignal && a.buySignal == b.buySignal && a.sellSignal == b.sellSignal);
    assert(a.buyScore == b.buyScore && a.sellScore == b.sellScore);
    assert(a.trend == b.trend && a.priceStructure == b.priceStructure);
    assert(a.nearestSupport == b.nearestSupport && a.nearestResistance == b.nearestResistance);
    assert(a.secondSupport == b.secondSupport && a.secondResistance == b.secondResistance);
    assert(a.buyConditions == b.buyConditions && a.sellConditions == b.sellConditions);
    assert(a.reasons == b.reasons && a.macdHist == b.macdHist);
    assert(a.breakout == b.breakout && a.confirmedBreakout == b.confirmedBreakout);
    assert(a.swingLows.size() == b.swingLows.size() && a.swingHighs.size() == b.swingHighs.size());
}

void tests() {
    TechnicalSwingConfig config;
    config.enableTechnicalSwingStrategy = true;
    TechnicalSwingStrategy strategy(config);
    assert(strategy.analyze({}).technicalSignal == "INSUFFICIENT_DATA");
    assert(strategy.analyze(waves(119)).technicalSignal == "INSUFFICIENT_DATA");
    assert(strategy.analyze(waves(120)).technicalSignal != "INSUFFICIENT_DATA");
    assert(TechnicalSwingStrategy{}.analyze(waves()).technicalSignal == "DISABLED");

    // 1. 持续下跌（包括没有局部低点的单边跌势）不能产生买入状态。
    auto falling = waves(180);
    for (size_t i = 0; i < falling.size(); ++i) {
        double p = 30 - 0.0005 * i * i;
        falling[i] = {date(i), p + 0.05, p, p + 0.1, p - 0.1, 1000};
    }
    for (const auto& r : strategy.analyzeHistory(falling)) assert(!buying(r.technicalSignal));
    // 带已确认 lower low 的下跌，再次加速，必须命中禁止摊平过滤。
    auto decliningWaves = waves(180, -0.02);
    append(decliningWaves, 6.0, 5.0, 6.1, 4.9, 1600);
    auto blocked = strategy.analyze(decliningWaves);
    assert(blocked.lowerLow && blocked.buyBlocked && !buying(blocked.buySignal));

    // 2. 支撑区域的抬高低点和MA10改善各自进入评分；必须实际出现组合。
    auto bottoming = waves(200, 0.003);
    for (auto& b : bottoming) {
        b.open = 10 + (b.open - 10) * 0.1;
        b.close = 10 + (b.close - 10) * 0.1;
        b.high = 10 + (b.high - 10) * 0.1;
        b.low = 10 + (b.low - 10) * 0.1;
    }
    bool foundRecovery = false;
    for (const auto& r : strategy.analyzeHistory(bottoming)) {
        if (r.higherLow && r.nearSupport && r.close > r.ma10) {
            assert(r.buyConditions.at("higher_low") && r.buyConditions.at("above_ma10"));
            assert(r.buyConditions.at("near_support") && r.buyScore >= 3);
            foundRecovery = true;
        }
    }
    assert(foundRecovery);

    // 3. 压力11.1，多次测试；突破当天和次日沿用同一原压力基准。
    auto breakout = waves();
    append(breakout, 11.39, 11.4, 12.0, 9.9, 2200);
    auto first = strategy.analyze(breakout);
    assert(first.breakout && !first.confirmedBreakout && first.technicalSignal == "BREAKOUT_HOLD");
    assert(first.sellScore >= 2);  // 长上影与放量滞涨不能覆盖有效突破优先级。
    assert(first.breakoutLevel && std::abs(*first.breakoutLevel - 11.1) < 1e-8);
    append(breakout, 11.4, 11.5, 11.6, 11.3, 1000);
    auto second = strategy.analyze(breakout);
    assert(second.confirmedBreakout && second.technicalSignal == "BREAKOUT_HOLD");
    auto lateVolume = waves();
    append(lateVolume, 10.0, 11.4, 11.5, 9.9, 500);
    assert(!strategy.analyze(lateVolume).breakout);
    append(lateVolume, 11.4, 11.5, 11.6, 11.3, 2500);
    assert(strategy.analyze(lateVolume).breakout);
    append(lateVolume, 11.4, 10.9, 11.7, 10.8, 3000);
    auto failedContinuation = strategy.analyze(lateVolume);
    assert(failedContinuation.falseBreakout && !failedContinuation.breakout);
    auto twoLevels = waves();
    for (size_t i = 0; i < 80; ++i) {
        twoLevels[i].open += 2; twoLevels[i].close += 2;
        twoLevels[i].high += 2; twoLevels[i].low += 2;
    }
    append(twoLevels, 11.3, 11.4, 11.5, 11.2, 2200);
    assert(strategy.analyze(twoLevels).breakout);
    append(twoLevels, 13.0, 13.0, 13.6, 12.9, 2500);
    auto upperFailure = strategy.analyze(twoLevels);
    assert(upperFailure.falseBreakout && !upperFailure.breakout && upperFailure.sellScore >= 3);

    // 4. 假突破+长上影+放量滞涨，提高减仓评分。
    auto fake = waves(162);
    append(fake, 10.4, 10.35, 11.6, 10.2, 2600);
    auto falseResult = strategy.analyze(fake);
    assert(falseResult.falseBreakout && falseResult.longUpperShadow && falseResult.volumeStagnation);
    assert(falseResult.sellScore >= 3 && !falseResult.breakout);

    // 5. 横盘回升中的MACD金叉缺少量价结构确认，不能成为买入确认。
    bool foundCross = false;
    for (const auto& r : strategy.analyzeHistory(waves(240))) {
        if (r.macdGoldenCross) {
            foundCross = true;
            assert(!buying(r.technicalSignal));
        }
    }
    assert(foundCross);

    // 6. 盘中跌破8.9但收回，区别于放量收盘跌破。
    auto recovered = waves();
    append(recovered, 9.1, 9.2, 9.3, 8.5, 1000);
    auto recovery = strategy.analyze(recovered);
    assert(recovery.supportRecovered && !recovery.supportBreakdown);
    auto broken = waves();
    append(broken, 9.1, 8.6, 9.2, 8.5, 2500);
    auto breakdown = strategy.analyze(broken);
    assert(breakdown.supportBreakdown && breakdown.buyBlocked && !breakdown.supportRecovered);

    // 7. 每个历史点等于独立前缀分析，未来极端行情不能修改历史。
    auto data = waves(175, 0.001);
    auto history = strategy.analyzeHistory(data);
    for (size_t n = 1; n <= data.size(); ++n) {
        auto prefix = strategy.analyzeHistory({data.begin(), data.begin() + n});
        equivalent(history[n - 1], prefix.back());
        for (const auto& p : history[n - 1].swingLows) {
            assert(p.confirmedIndex == p.index + config.swingWindow && p.confirmedIndex < n);
        }
    }
    append(data, 100, 150, 200, 0.1, 100000);
    auto extended = strategy.analyzeHistory(data);
    for (size_t i = 0; i < history.size(); ++i) equivalent(history[i], extended[i]);
    auto plateau = waves();
    for (auto& b : plateau) b = {b.date, 10, 10, 10, 10, 0};
    auto flat = strategy.analyze(plateau);
    assert(flat.swingLows.empty() && flat.swingHighs.empty() && !flat.nearestSupport);
    assert(std::isfinite(flat.volumeRatio) && flat.volumeRatio == 0);
    auto invalid = waves(); invalid.back().close = std::numeric_limits<double>::quiet_NaN();
    assert(strategy.analyze(invalid).technicalSignal == "INVALID_DATA");
    invalid = waves(); invalid.back().date = invalid.front().date;
    assert(strategy.analyze(invalid).technicalSignal == "INVALID_DATA");
    auto badConfig = config; badConfig.maPeriods[0] = 0;
    assert(TechnicalSwingStrategy(badConfig).analyze(waves()).technicalSignal == "INVALID_CONFIG");

    // 参数完整往返，旧三列配置保持关闭，非法配置不能部分生效。
    config.rejectionPct = 0.021; config.maPeriods = {4, 9, 21};
    TechnicalSwingConfig restored;
    assert(parseTechnicalSwingConfig(serializeTechnicalSwingConfig(config), restored));
    assert(serializeTechnicalSwingConfig(config) == serializeTechnicalSwingConfig(restored));
    assert(!parseTechnicalSwingConfig("swingWindow=-1;", restored));
    auto path = std::filesystem::temp_directory_path() / "ashare_swing_config_test.tsv";
    StrategyConfigStore store(path, path.string() + ".active");
    StrategyConfig saved{"test", "test", StrategyKind::Breakout}; saved.technicalSwing = config;
    store.save({saved});
    assert(serializeTechnicalSwingConfig(store.load().front().technicalSwing) == serializeTechnicalSwingConfig(config));
    { std::ofstream out(path); out << "test\ttest\tbreakout\n"; }
    assert(!store.load().front().technicalSwing.enableTechnicalSwingStrategy);
    std::filesystem::remove(path);

    // 原最终信号不变，适配器只附加因子，成本对因子无影响。
    auto bars = waves();
    Quote quote; quote.symbol = "fixture"; quote.price = bars.back().close;
    Holding h; h.quantity = 1000; h.cost = 1;
    auto factor = strategy.evaluate(quote, bars, {}, &h);
    h.cost = 100;
    auto otherCost = strategy.evaluate(quote, bars, {}, &h);
    assert(factor.type == SignalType::None);
    equivalent(*factor.technicalSwingResult, *otherCost.technicalSwingResult);
    for (const auto& base : defaultStrategies()) {
        auto off = createTradingAlgorithm(base);
        auto onConfig = base; onConfig.technicalSwing.enableTechnicalSwingStrategy = true;
        auto on = createTradingAlgorithm(onConfig);
        std::vector<KLine> legacy(bars.end() - 80, bars.end());
        auto a = off->evaluate(quote, legacy, bars, &h), b = on->evaluate(quote, bars, bars, &h);
        assert(!a.technicalSwingResult && b.technicalSwingResult);
        assert(a.type == b.type && a.message == b.message && a.algorithm == b.algorithm);
        assert(a.suggestedQuantity == b.suggestedQuantity && a.windowLow == b.windowLow && a.windowHigh == b.windowHigh);
    }
    std::cout << "Technical swing tests passed\n";
}

void example(const char* path, const char* symbol) {
    std::ifstream input(path);
    assert(input);
    std::vector<KLine> bars;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        KLine b; std::istringstream row(line);
        if (row >> b.date >> b.open >> b.close >> b.high >> b.low >> b.volume) bars.push_back(b);
    }
    TechnicalSwingConfig config; config.enableTechnicalSwingStrategy = true;
    auto r = TechnicalSwingStrategy(config).analyze(bars, symbol);
    std::cout << "TechnicalSwingStrategy\nSymbol: " << symbol << "\nDate: " << r.date
              << "\nBars: " << bars.size() << "\nTrend: " << r.trend << "\nStructure: " << r.priceStructure
              << "\nBuy Score: " << r.buyScore << "/7\nSell Score: " << r.sellScore << "/6\nSignal: " << r.technicalSignal
              << "\nSupport: " << (r.nearestSupport ? std::to_string(*r.nearestSupport) : "N/A")
              << "\nResistance: " << (r.nearestResistance ? std::to_string(*r.nearestResistance) : "N/A") << '\n';
    for (const auto& reason : r.reasons) std::cout << "- " << reason << '\n';
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3) example(argv[1], argv[2]);
    else tests();
}
