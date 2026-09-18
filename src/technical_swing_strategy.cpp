#include "technical_swing_strategy.h"
#include "technical_indicators.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <type_traits>
#include <utility>

namespace ashare {
namespace {

bool validConfig(const TechnicalSwingConfig& c) {
    const double values[] = {c.supportResistanceTolerance, c.supportZoneDistance,
        c.resistanceZoneDistance, c.volumeExpandRatio, c.volumeContractRatio,
        c.breakoutVolumeRatio, c.breakoutConfirmPct, c.upperShadowRatio,
        c.minimumBodyPct, c.rejectionPct, c.stagnationPct, c.highOpenPct,
        c.lowCloseFraction, c.supportBreakdownPct};
    for (double v : values) if (!std::isfinite(v) || v < 0.0) return false;
    return c.maPeriods[0] > 0 && c.maPeriods[0] < c.maPeriods[1] &&
        c.maPeriods[1] < c.maPeriods[2] && c.maPeriods[2] <= 10000 &&
        c.slopeDays > 0 && c.slopeDays <= 10000 && c.macdFast > 0 &&
        c.macdFast < c.macdSlow && c.macdSlow <= 10000 && c.macdSignal > 0 &&
        c.macdSignal <= 10000 && c.swingWindow > 0 && c.swingWindow <= 10000 &&
        c.supportResistanceLookback > 2 * c.swingWindow && c.minimumLevelTests >= 2 &&
        c.volumeFast > 0 && c.volumeSlow >= c.volumeFast && c.volumeSlow <= 10000 &&
        c.noNewLowDays > 0 && c.noNewLowDays <= 10000 &&
        c.breakoutConfirmDays > 0 && c.breakoutConfirmDays <= 10000 &&
        c.volumeExpandRatio > 1.0 && c.volumeContractRatio < 1.0 &&
        c.breakoutVolumeRatio >= 1.0 && c.minimumBodyPct > 0.0 &&
        c.supportResistanceTolerance < 1.0 && c.supportBreakdownPct < 1.0 &&
        c.lowCloseFraction <= 1.0 && c.falseBreakoutWeight >= 0 && c.falseBreakoutWeight <= 6;
}

bool validBar(const KLine& b) {
    return !b.date.empty() && std::isfinite(b.open) && std::isfinite(b.high) &&
        std::isfinite(b.low) && std::isfinite(b.close) && std::isfinite(b.volume) &&
        b.low > 0.0 && b.volume >= 0.0 && b.high >= std::max(b.open, b.close) &&
        b.low <= std::min(b.open, b.close);
}

struct Level { double price; size_t tests; };
std::vector<Level> levels(const std::vector<SwingPoint>& points, size_t t,
                          const TechnicalSwingConfig& c) {
    std::vector<double> prices;
    for (const auto& p : points) {
        // 当日刚确认的极值不改变当日盘前支撑/压力基准。
        if (p.confirmedIndex < t && t - p.index <= c.supportResistanceLookback)
            prices.push_back(p.price);
    }
    std::sort(prices.begin(), prices.end());
    std::vector<Level> result;
    for (size_t i = 0; i < prices.size();) {
        size_t j = i + 1;
        double sum = prices[i];
        // 使用簇最小值限制总宽度，防止链式聚类把远端价格连成一区。
        while (j < prices.size() && prices[j] / prices[i] - 1.0 <= c.supportResistanceTolerance)
            sum += prices[j++];
        if (j - i >= c.minimumLevelTests) result.push_back({sum / (j - i), j - i});
        i = j;
    }
    return result;
}

void logResult(const TechnicalSwingResult& r, bool debug) {
    std::ostringstream out;
    out << "technical_swing symbol=" << r.symbol << " date=" << r.date
        << " buy_score=" << r.buyScore << " sell_score=" << r.sellScore
        << " signal=" << r.technicalSignal << " trend=" << r.trend
        << " support=" << (r.nearestSupport ? std::to_string(*r.nearestSupport) : "NA")
        << " resistance=" << (r.nearestResistance ? std::to_string(*r.nearestResistance) : "NA");
    if (debug) {
        for (const auto& [key, value] : r.buyConditions) out << " buy." << key << '=' << value;
        for (const auto& [key, value] : r.sellConditions) out << " sell." << key << '=' << value;
    }
    logger().write(debug ? "DEBUG" : "INFO", out.str());
}

}  // namespace

TechnicalSwingStrategy::TechnicalSwingStrategy(TechnicalSwingConfig config) : config_(config) {}

// 序列化与解析共用字段表，避免参数新增后持久化遗漏。
#define SWING_FIELDS(X) \
    X(enableTechnicalSwingStrategy) X(debug) X(showChartOverlay) X(minimumData) \
    X(slopeDays) X(macdFast) X(macdSlow) X(macdSignal) X(swingWindow) \
    X(supportResistanceLookback) X(minimumLevelTests) X(supportResistanceTolerance) \
    X(supportZoneDistance) X(resistanceZoneDistance) X(volumeFast) X(volumeSlow) \
    X(volumeExpandRatio) X(volumeContractRatio) X(breakoutVolumeRatio) X(breakoutConfirmPct) \
    X(breakoutConfirmDays) X(upperShadowRatio) X(minimumBodyPct) X(rejectionPct) \
    X(stagnationPct) X(highOpenPct) X(lowCloseFraction) X(supportBreakdownPct) \
    X(noNewLowDays) X(falseBreakoutWeight)

std::string serializeTechnicalSwingConfig(const TechnicalSwingConfig& c) {
    std::ostringstream out;
    out << std::setprecision(17);
#define WRITE_FIELD(field) out << #field << '=' << c.field << ';';
    SWING_FIELDS(WRITE_FIELD)
#undef WRITE_FIELD
    for (size_t i = 0; i < 3; ++i) out << "maPeriod" << i << '=' << c.maPeriods[i] << ';';
    return out.str();
}

bool parseTechnicalSwingConfig(const std::string& text, TechnicalSwingConfig& config) {
    TechnicalSwingConfig candidate;
    try {
        std::istringstream in(text);
        std::string token;
        while (std::getline(in, token, ';')) {
            if (token.empty()) continue;
            auto separator = token.find('=');
            if (separator == std::string::npos) return false;
            std::string key = token.substr(0, separator), value = token.substr(separator + 1);
            size_t used = 0;
            double number = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(number) || number < 0 || number > 1000000) return false;
            auto assign = [&](auto& field) {
                using T = std::decay_t<decltype(field)>;
                if constexpr (std::is_integral_v<T>) {
                    if (std::floor(number) != number) return false;
                    if constexpr (std::is_same_v<T, bool>) if (number > 1) return false;
                }
                field = static_cast<T>(number);
                return true;
            };
            bool known = false;
            if (key == "enable_technical_swing_strategy") key = "enableTechnicalSwingStrategy";
#define READ_FIELD(field) if (key == #field) { if (!assign(candidate.field)) return false; known = true; }
            SWING_FIELDS(READ_FIELD)
#undef READ_FIELD
            for (size_t i = 0; i < 3; ++i) if (key == "maPeriod" + std::to_string(i)) {
                if (!assign(candidate.maPeriods[i])) return false;
                known = true;
            }
            if (!known) return false;
        }
    } catch (...) { return false; }
    if (!validConfig(candidate)) return false;
    config = candidate;
    return true;
}
#undef SWING_FIELDS

std::vector<TechnicalSwingResult> TechnicalSwingStrategy::analyzeHistory(
    const std::vector<KLine>& bars, const std::string& symbol) const {
    std::vector<TechnicalSwingResult> history;
    history.reserve(bars.size());
    const auto& c = config_;
    const bool configValid = validConfig(c);
    std::vector<MacdValues> macd;
    if (configValid) macd = calculateMacdHistory(bars, c.macdFast, c.macdSlow, c.macdSignal);
    std::vector<SwingPoint> lows, highs;
    std::optional<double> activeBreakout;
    std::optional<double> pendingResistance;
    size_t breakoutDays = 0;
    bool invalid = false;
    auto average = [&](size_t t, size_t period, bool volume = false) {
        double sum = 0.0;
        for (size_t i = t + 1 - period; i <= t; ++i)
            sum += volume ? bars[i].volume : bars[i].close;
        return sum / period;
    };
    for (size_t t = 0; t < bars.size(); ++t) {
        TechnicalSwingResult r;
        r.symbol = symbol;
        r.date = bars[t].date;
        if (!c.enableTechnicalSwingStrategy) {
            r.technicalSignal = "DISABLED";
            history.push_back(std::move(r));
            continue;
        }
        invalid = invalid || !validBar(bars[t]) || (t > 0 && bars[t].date <= bars[t - 1].date);
        if (!configValid || invalid) {
            r.technicalSignal = configValid ? "INVALID_DATA" : "INVALID_CONFIG";
            r.reasons.push_back(configValid ? "日线必须按日期严格升序，OHLCV必须有限且有效" : "技术策略参数无效");
            history.push_back(std::move(r));
            continue;
        }
        // i 的极值仅在 t=i+window 时确认；平台只接受最后一个相等极值。
        if (t >= 2 * c.swingWindow) {
            size_t i = t - c.swingWindow;
            bool low = true, high = true;
            for (size_t j = i - c.swingWindow; j <= t; ++j) {
                if (j == i) continue;
                low = low && (j < i ? bars[i].low <= bars[j].low : bars[i].low < bars[j].low);
                high = high && (j < i ? bars[i].high >= bars[j].high : bars[i].high > bars[j].high);
            }
            if (low) lows.push_back({i, t, bars[i].date, bars[t].date, bars[i].low});
            if (high) highs.push_back({i, t, bars[i].date, bars[t].date, bars[i].high});
        }
        auto prune = [&](std::vector<SwingPoint>& points) {
            points.erase(std::remove_if(points.begin(), points.end(), [&](const SwingPoint& p) {
                return t - p.index > c.supportResistanceLookback;
            }), points.end());
        };
        prune(lows); prune(highs);
        size_t required = std::max({c.minimumData, c.maPeriods[2] + c.slopeDays,
            c.macdSlow + c.macdSignal + 2, c.volumeSlow, 2 * c.swingWindow + 1,
            c.noNewLowDays + 1});
        if (t + 1 < required) {
            r.reasons.push_back("有效日线不足：需要" + std::to_string(required) +
                                "根，当前" + std::to_string(t + 1) + "根");
            history.push_back(std::move(r));
            continue;
        }
        const auto& b = bars[t];
        const auto& prev = bars[t - 1];
        r.close = b.close;
        r.ma5 = average(t, c.maPeriods[0]);
        r.ma10 = average(t, c.maPeriods[1]);
        r.ma20 = average(t, c.maPeriods[2]);
        r.ma5Slope = (r.ma5 - average(t - c.slopeDays, c.maPeriods[0])) / c.slopeDays;
        r.ma10Slope = (r.ma10 - average(t - c.slopeDays, c.maPeriods[1])) / c.slopeDays;
        r.ma20Slope = (r.ma20 - average(t - c.slopeDays, c.maPeriods[2])) / c.slopeDays;
        r.bearAlignment = r.ma5 < r.ma10 && r.ma10 < r.ma20;
        r.strongBearTrend = r.bearAlignment && r.ma5Slope < 0 && r.ma10Slope < 0 && r.ma20Slope < 0;
        r.shortRecovery = b.close > r.ma10 && r.ma5Slope > 0;
        r.shortBullish = r.ma5 > r.ma10 && r.ma5Slope > 0 && r.ma10Slope > 0;
        r.aboveMa20 = b.close > r.ma20;
        r.bullAlignment = r.ma5 > r.ma10 && r.ma10 > r.ma20;
        r.dif = macd[t].dif; r.dea = macd[t].dea; r.macdHist = macd[t].histogram;
        r.macdGoldenCross = r.dif > r.dea && macd[t - 1].dif <= macd[t - 1].dea;
        r.macdDeadCross = r.dif < r.dea && macd[t - 1].dif >= macd[t - 1].dea;
        r.macdHistIncreasing = r.macdHist > macd[t - 1].histogram;
        r.macdHistDecreasing = r.macdHist < macd[t - 1].histogram;
        r.greenHistShrinking = r.macdHist < 0 && r.macdHistIncreasing && macd[t - 1].histogram < 0;
        r.redHistShrinking = r.macdHist > 0 && macd[t - 1].histogram > r.macdHist &&
                            macd[t - 2].histogram > macd[t - 1].histogram;
        r.difTurningUp = r.dif > macd[t - 1].dif && macd[t - 1].dif <= macd[t - 2].dif;
        r.difTurningDown = r.dif < macd[t - 1].dif && macd[t - 1].dif >= macd[t - 2].dif;
        r.volMa5 = average(t, c.volumeFast, true);
        r.volMa10 = average(t, c.volumeSlow, true);
        r.volumeRatio = r.volMa5 > 0 ? b.volume / r.volMa5 : 0.0;
        r.volumeExpansion = r.volMa5 > 0 && r.volumeRatio >= c.volumeExpandRatio;
        r.volumeContraction = r.volMa5 > 0 && r.volumeRatio < c.volumeContractRatio;
        r.bullishVolumeConfirmation = b.close > prev.close && r.volumeExpansion;
        r.sellingPressureDeclining = b.close < prev.close && r.volumeContraction;
        r.swingLows = lows; r.swingHighs = highs;
        if (lows.size() >= 2) {
            r.higherLow = lows.back().price > lows[lows.size() - 2].price;
            r.lowerLow = lows.back().price < lows[lows.size() - 2].price;
        }
        if (highs.size() >= 2) {
            r.higherHigh = highs.back().price > highs[highs.size() - 2].price;
            r.lowerHigh = highs.back().price < highs[highs.size() - 2].price;
        }
        r.noNewLow = true;
        // 最近 N 日均未跌破此前的锚点低点，而非仅判断今天没创新低。
        for (size_t i = t - c.noNewLowDays + 1; i <= t; ++i)
            r.noNewLow = r.noNewLow && bars[i].low >= bars[t - c.noNewLowDays].low;
        r.breakRecentSwingHigh = !highs.empty() && b.close > highs.back().price &&
                                prev.close <= highs.back().price;
        r.breakoutVolumeConfirmed = r.breakRecentSwingHigh && r.volumeRatio >= c.breakoutVolumeRatio;
        auto supports = levels(lows, t, c), resistances = levels(highs, t, c);
        for (auto it = supports.rbegin(); it != supports.rend(); ++it) {
            if (it->price <= b.close) {
                if (!r.nearestSupport) r.nearestSupport = it->price;
                else if (!r.secondSupport) r.secondSupport = it->price;
            }
            if (!r.testedSupport && it->price <= prev.close) r.testedSupport = it->price;
        }
        std::optional<double> testedResistance;
        for (const auto& level : resistances) {
            if (level.price >= b.close) {
                if (!r.nearestResistance) r.nearestResistance = level.price;
                else if (!r.secondResistance) r.secondResistance = level.price;
            }
            if (!testedResistance && level.price >= prev.close) testedResistance = level.price;
        }
        // 首次越过压力但量能不足时保留基准，允许后续补量确认；
        // 跌回原压力时也必须用原基准检查失败突破。
        if (pendingResistance && testedResistance &&
            *testedResistance > *pendingResistance * (1 + c.supportResistanceTolerance) &&
            b.high >= *testedResistance * (1 - c.resistanceZoneDistance)) {
            // 已到达更高的一层压力，旧突破不能永久屏蔽这里的新转弱信号。
            pendingResistance.reset(); activeBreakout.reset(); breakoutDays = 0;
        }
        if (pendingResistance) testedResistance = pendingResistance;
        if (r.nearestSupport) {
            r.distanceToSupportPct = (b.close - *r.nearestSupport) / *r.nearestSupport;
            r.nearSupport = *r.distanceToSupportPct <= c.supportZoneDistance;
        }
        if (r.nearestResistance) {
            r.distanceToResistancePct = (*r.nearestResistance - b.close) / b.close;
            r.nearResistance = *r.distanceToResistancePct <= c.resistanceZoneDistance;
        }
        double body = std::abs(b.close - b.open);
        double upper = b.high - std::max(b.open, b.close);
        r.longUpperShadow = upper >= std::max(body, b.close * c.minimumBodyPct) * c.upperShadowRatio;
        bool rejection = (b.high - b.close) / b.close >= c.rejectionPct;
        r.volumeStagnation = r.volumeExpansion &&
            (std::abs(b.close / prev.close - 1.0) <= c.stagnationPct || rejection);
        r.bearishEngulfing = b.close < b.open && prev.close > prev.open &&
                            b.open >= prev.close && b.close <= prev.open;
        r.bullishEngulfing = b.close > b.open && prev.close < prev.open &&
                            b.open <= prev.close && b.close >= prev.open;
        r.highOpenLowClose = b.open >= prev.close * (1 + c.highOpenPct) && b.close < b.open &&
                            b.close <= b.low + (b.high - b.low) * c.lowCloseFraction;
        r.rejectionFromResistance = testedResistance && b.high >= *testedResistance &&
                                    b.close <= *testedResistance && rejection;
        r.falseBreakout = testedResistance && b.high > *testedResistance && b.close <= *testedResistance &&
                          (r.longUpperShadow || r.volumeStagnation);
        if (testedResistance && b.close > *testedResistance) pendingResistance = testedResistance;
        else pendingResistance.reset();
        if (activeBreakout && b.close < *activeBreakout * (1 + c.breakoutConfirmPct)) {
            activeBreakout.reset(); breakoutDays = 0;
        }
        if (testedResistance && b.close >= *testedResistance * (1 + c.breakoutConfirmPct) &&
            b.close > *testedResistance && r.volumeRatio >= c.breakoutVolumeRatio) {
            if (!activeBreakout || *activeBreakout != *testedResistance) breakoutDays = 0;
            activeBreakout = testedResistance;
        }
        if (activeBreakout) {
            ++breakoutDays;
            r.breakout = true;
            r.breakoutLevel = activeBreakout;
            r.confirmedBreakout = breakoutDays >= c.breakoutConfirmDays;
        }
        r.supportRecovered = r.testedSupport && b.low < *r.testedSupport && b.close > *r.testedSupport;
        r.supportBreakdown = r.testedSupport && b.close < *r.testedSupport * (1 - c.supportBreakdownPct) &&
                             r.volumeExpansion;
        r.buyConditions = {{"near_support", r.nearSupport}, {"no_new_low", r.noNewLow},
            {"higher_low", r.higherLow}, {"above_ma10", b.close > r.ma10},
            {"volume_improving", r.sellingPressureDeclining || r.bullishVolumeConfirmation},
            {"macd_improving", r.greenHistShrinking || r.macdGoldenCross || r.difTurningUp},
            {"break_recent_swing_high", r.breakRecentSwingHigh}};
        r.sellConditions = {{"near_resistance", r.nearResistance},
            {"weak_candle", r.longUpperShadow || r.bearishEngulfing || r.rejectionFromResistance || r.highOpenLowClose},
            {"volume_stagnation", r.volumeStagnation},
            {"cross_below_ma5", prev.close >= average(t - 1, c.maPeriods[0]) && b.close < r.ma5},
            {"red_hist_shrinking", r.redHistShrinking}, {"macd_dead_cross", r.macdDeadCross}};
        for (const auto& [key, value] : r.buyConditions) if (value) {
            ++r.buyScore; r.reasons.push_back("低吸 +1: " + key);
        }
        for (const auto& [key, value] : r.sellConditions) if (value) {
            ++r.sellScore; r.reasons.push_back("高抛 +1: " + key);
        }
        if (r.falseBreakout) {
            r.sellScore = std::min(6, r.sellScore + c.falseBreakoutWeight);
            r.reasons.push_back("压力位假突破，增加高抛权重");
        }
        r.buyBlocked = (b.close < r.ma5 && r.bearAlignment && r.lowerLow &&
                        r.macdHist < 0 && r.macdHistDecreasing) || r.supportBreakdown;
        r.buyStrength = r.buyScore / 7.0;
        r.sellStrength = r.sellScore / 6.0;
        static const char* buySignals[] = {"WAIT", "WAIT", "WAIT", "WATCH_BUY", "EARLY_BUY",
                                           "BUY_CONFIRMATION", "STRONG_BUY_CONFIRMATION", "STRONG_BUY_CONFIRMATION"};
        static const char* sellSignals[] = {"HOLD", "HOLD", "WATCH_SELL", "PARTIAL_SELL",
                                            "SELL_CONFIRMATION", "STRONG_SELL_CONFIRMATION", "STRONG_SELL_CONFIRMATION"};
        r.buySignal = buySignals[r.buyScore]; r.sellSignal = sellSignals[r.sellScore];
        // 分数保留用于上层组合，但买入类状态要求价格结构、均线和量能共同确认。
        bool buyReady = r.noNewLow && (r.higherLow || r.breakRecentSwingHigh) &&
            r.shortRecovery && r.buyConditions.at("volume_improving") && !r.strongBearTrend;
        if (r.buyScore >= 4 && !buyReady) {
            r.buySignal = "WATCH_BUY";
            r.reasons.push_back("评分达到候选阈值，但结构/均线/量能尚未共同确认，继续观察");
        }
        if (r.buyBlocked) {
            r.buySignal = "DO_NOT_AVERAGE_DOWN";
            r.reasons.push_back("下降趋势仍在延续，不应仅因为价格下降进行补仓。");
        }
        r.priceStructure = r.higherLow && r.higherHigh ? "UPTREND" :
            r.lowerLow && r.lowerHigh ? "DOWNTREND" : r.higherLow ? "EARLY_REVERSAL" :
            r.noNewLow && r.nearSupport ? "BOTTOMING" : r.lowerHigh && r.nearResistance ? "TOPPING" :
            r.nearSupport ? "POSSIBLE_BOTTOM" : "RANGE";
        r.trend = r.strongBearTrend ? "CONTINUED_DECLINE" :
            r.higherLow && r.higherHigh && r.bullAlignment ? "TREND_STRENGTHENING" :
            r.higherLow && r.shortRecovery ? "EARLY_STRENGTHENING" :
            r.noNewLow && r.nearSupport ? "BOTTOMING" : r.nearSupport ? "BOTTOM_WATCH" :
            r.nearResistance && r.sellScore >= 3 ? "HIGH_WEAKENING" :
            r.nearResistance ? "NEAR_RESISTANCE" : "HOLD";
        r.technicalSignal = r.sellScore >= 2 ? r.sellSignal : r.buySignal;
        if (r.buyBlocked && r.sellScore < 2) r.technicalSignal = "DO_NOT_AVERAGE_DOWN";
        if (r.breakout && !r.buyBlocked) {
            r.technicalSignal = "BREAKOUT_HOLD";
            r.trend = "BREAKOUT";
            r.reasons.push_back(r.confirmedBreakout ? "放量突破后持续站稳原压力位，优先持有观察" :
                                                     "放量收盘突破原压力位，等待持续站稳，避免机械高抛");
        }
        if (r.supportRecovered) r.reasons.push_back("盘中跌破支撑但收盘收回，不认定有效破位");
        if (r.supportBreakdown) r.reasons.push_back("放量收盘有效跌破原支撑，阻止低吸");
        if (r.higherLow && r.higherHigh) r.reasons.push_back("高低点同时抬高，原下降结构被破坏（优先于MACD）");
        if (r.breakoutVolumeConfirmed) r.reasons.push_back("突破最近波段高点获得成交量确认");
        if (r.reasons.empty()) r.reasons.push_back("缺少结构与量价共振，等待");
        history.push_back(std::move(r));
    }
    return history;
}

TechnicalSwingResult TechnicalSwingStrategy::analyze(const std::vector<KLine>& daily,
                                                     const std::string& symbol) const {
    auto history = analyzeHistory(daily, symbol);
    TechnicalSwingResult result;
    if (!history.empty()) result = std::move(history.back());
    else {
        result.symbol = symbol;
        result.technicalSignal = !config_.enableTechnicalSwingStrategy ? "DISABLED" :
                                  !validConfig(config_) ? "INVALID_CONFIG" : "INSUFFICIENT_DATA";
        result.reasons.push_back("没有日线数据");
    }
    if (config_.enableTechnicalSwingStrategy) logResult(result, config_.debug);
    return result;
}

TradingSignal TechnicalSwingStrategy::evaluate(const Quote& quote, const std::vector<KLine>& daily,
                                               const std::vector<KLine>&, const Holding*) const {
    TradingSignal signal;
    signal.symbol = quote.symbol; signal.name = quote.name; signal.algorithm = name();
    signal.time = quote.time; signal.currentPrice = quote.price;
    if (config_.enableTechnicalSwingStrategy)
        signal.technicalSwingResult = std::make_shared<TechnicalSwingResult>(analyze(daily, quote.symbol));
    return signal;
}

namespace {
class TechnicalSwingDecorator final : public TradingAlgorithm {
public:
    TechnicalSwingDecorator(std::unique_ptr<TradingAlgorithm> base, TechnicalSwingConfig config)
        : base_(std::move(base)), factor_(config) {}
    std::string name() const override { return base_->name(); }
    TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                           const std::vector<KLine>& minute, const Holding* holding) const override {
        // 原系统调用约定是最近80根，扩充因子历史不能改变原算法输入。
        std::vector<KLine> legacy = daily;
        if (legacy.size() > 80) legacy.erase(legacy.begin(), legacy.end() - 80);
        auto result = base_->evaluate(quote, legacy, minute, holding);
        result.technicalSwingResult = std::make_shared<TechnicalSwingResult>(factor_.analyze(daily, quote.symbol));
        return result;
    }
private:
    std::unique_ptr<TradingAlgorithm> base_;
    TechnicalSwingStrategy factor_;
};
}  // namespace

std::unique_ptr<TradingAlgorithm> withTechnicalSwing(std::unique_ptr<TradingAlgorithm> algorithm,
                                                     const TechnicalSwingConfig& config) {
    if (!config.enableTechnicalSwingStrategy) return algorithm;
    return std::make_unique<TechnicalSwingDecorator>(std::move(algorithm), config);
}

}  // namespace ashare
