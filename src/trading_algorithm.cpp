#include "trading_algorithm.h"

#include "utils.h"
#include "technical_swing_strategy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace ashare {

std::string strategyKindKey(StrategyKind kind) {
    switch (kind) {
    case StrategyKind::Breakout: return "breakout";
    case StrategyKind::MeanReversion: return "mean_reversion";
    case StrategyKind::T0Intraday: return "t0_intraday";
    default: return "ma_momentum";
    }
}

std::optional<StrategyKind> strategyKindFromKey(const std::string& key) {
    if (key == "ma_momentum") return StrategyKind::MovingAverageMomentum;
    if (key == "breakout") return StrategyKind::Breakout;
    if (key == "mean_reversion") return StrategyKind::MeanReversion;
    if (key == "t0_intraday") return StrategyKind::T0Intraday;
    return std::nullopt;
}

std::vector<StrategyConfig> defaultStrategies() {
    return {
        {"builtin_ma_momentum", "均线动量", StrategyKind::MovingAverageMomentum},
        {"builtin_breakout", "趋势突破", StrategyKind::Breakout},
        {"builtin_mean_reversion", "均值回归", StrategyKind::MeanReversion},
        {"builtin_t0_intraday", "T0 日内回转", StrategyKind::T0Intraday},
    };
}

// 5/20 日均线交叉结合最近 5 分钟动量，过滤只有日线交叉但盘中未确认的信号。
class MovingAverageMomentumAlgorithm final : public TradingAlgorithm {
public:
    explicit MovingAverageMomentumAlgorithm(std::string displayName) : displayName_(std::move(displayName)) {}
    std::string name() const override { return displayName_; }

    TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                           const std::vector<KLine>& minute, const Holding*) const override {
        TradingSignal signal;
        signal.symbol = quote.symbol;
        signal.name = quote.name;
        signal.algorithm = name();
        signal.currentPrice = quote.price;
        signal.time = quote.time;
        if (daily.size() < 21 || minute.size() < 6 || quote.price <= 0.0) {
            signal.message = "历史样本不足";
            return signal;
        }
        std::vector<double> closes;
        closes.reserve(daily.size());
        for (const auto& line : daily) {
            if (line.close > 0.0 && std::isfinite(line.close)) closes.push_back(line.close);
        }
        if (closes.size() < 21) {
            signal.message = "有效历史样本不足";
            return signal;
        }
        // 用实时价替换最后一根日线收盘价，让盘中评估反映当前走势。
        closes.back() = quote.price;
        auto averageEndingAt = [&](size_t endExclusive, size_t count) {
            double total = 0.0;
            for (size_t index = endExclusive - count; index < endExclusive; ++index) total += closes[index];
            return total / static_cast<double>(count);
        };
        size_t end = closes.size();
        double ma5 = averageEndingAt(end, 5);
        double ma20 = averageEndingAt(end, 20);
        double previousMa5 = averageEndingAt(end - 1, 5);
        double previousMa20 = averageEndingAt(end - 1, 20);
        double momentumBase = minute[minute.size() - 6].close;
        double momentum = momentumBase > 0.0 ? (minute.back().close / momentumBase - 1.0) * 100.0 : 0.0;
        double spread = ma20 > 0.0 ? (ma5 / ma20 - 1.0) * 100.0 : 0.0;
        // 除正式交叉外，也把均线接近且分钟动量同向的区域视为候选窗口。
        bool buyCrossover = previousMa5 <= previousMa20 && ma5 > ma20;
        bool sellCrossover = previousMa5 >= previousMa20 && ma5 < ma20;
        bool nearBuyWindow = spread >= 0.0 && spread <= 0.6 && momentum >= 0.15;
        bool nearSellWindow = spread <= 0.0 && spread >= -0.6 && momentum <= -0.15;
        if (buyCrossover || nearBuyWindow) {
            signal.type = SignalType::Buy;
            signal.windowLow = ma5 * 0.997;
            signal.windowHigh = ma5 * 1.003;
            signal.message = "短期均线转强且分钟动量向上";
        } else if (sellCrossover || nearSellWindow) {
            signal.type = SignalType::Sell;
            signal.windowLow = ma5 * 0.997;
            signal.windowHigh = ma5 * 1.003;
            signal.message = "短期均线转弱且分钟动量向下";
        } else {
            signal.message = "暂未进入交易窗口";
        }
        return signal;
    }

private:
    std::string displayName_;
};

// 比较当前价与之前 20 个交易日高低点，并用分钟动量确认突破方向。
class BreakoutAlgorithm final : public TradingAlgorithm {
public:
    explicit BreakoutAlgorithm(std::string displayName) : displayName_(std::move(displayName)) {}
    std::string name() const override { return displayName_; }

    TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                           const std::vector<KLine>& minute, const Holding*) const override {
        TradingSignal signal{quote.symbol, quote.name, name(), SignalType::None, quote.price,
                             0.0, 0.0, "", quote.time};
        if (daily.size() < 21 || minute.size() < 6 || quote.price <= 0.0) {
            signal.message = "历史样本不足";
            return signal;
        }
        // 排除最后一根（通常是今天），避免把当前价同时计入突破基准。
        size_t end = daily.size() - 1;
        size_t begin = end >= 20 ? end - 20 : 0;
        double highest = std::numeric_limits<double>::lowest();
        double lowest = std::numeric_limits<double>::max();
        for (size_t index = begin; index < end; ++index) {
            highest = std::max(highest, daily[index].high);
            lowest = std::min(lowest, daily[index].low);
        }
        double momentumBase = minute[minute.size() - 6].close;
        double momentum = momentumBase > 0.0 ? minute.back().close / momentumBase - 1.0 : 0.0;
        if (quote.price >= highest * 0.998 && momentum > 0.001) {
            signal.type = SignalType::Buy;
            signal.windowLow = highest * 0.998;
            signal.windowHigh = highest * 1.006;
            signal.message = "价格接近 20 日高点且分钟动量向上";
        } else if (quote.price <= lowest * 1.002 && momentum < -0.001) {
            signal.type = SignalType::Sell;
            signal.windowLow = lowest * 0.994;
            signal.windowHigh = lowest * 1.002;
            signal.message = "价格接近 20 日低点且分钟动量向下";
        } else {
            signal.message = "暂未形成有效突破";
        }
        return signal;
    }

private:
    std::string displayName_;
};

// 当价格明显偏离 20 日均线且短线开始反转时，寻找回归均值窗口。
class MeanReversionAlgorithm final : public TradingAlgorithm {
public:
    explicit MeanReversionAlgorithm(std::string displayName) : displayName_(std::move(displayName)) {}
    std::string name() const override { return displayName_; }

    TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                           const std::vector<KLine>& minute, const Holding*) const override {
        TradingSignal signal{quote.symbol, quote.name, name(), SignalType::None, quote.price,
                             0.0, 0.0, "", quote.time};
        if (daily.size() < 20 || minute.size() < 6 || quote.price <= 0.0) {
            signal.message = "历史样本不足";
            return signal;
        }
        double total = 0.0;
        for (size_t index = daily.size() - 20; index < daily.size(); ++index) total += daily[index].close;
        double ma20 = total / 20.0;
        double deviation = ma20 > 0.0 ? quote.price / ma20 - 1.0 : 0.0;
        double momentumBase = minute[minute.size() - 6].close;
        double momentum = momentumBase > 0.0 ? minute.back().close / momentumBase - 1.0 : 0.0;
        if (deviation <= -0.02 && momentum >= 0.0005) {
            signal.type = SignalType::Buy;
            signal.windowLow = ma20 * 0.970;
            signal.windowHigh = ma20 * 0.985;
            signal.message = "价格低于 20 日均线且短线出现回升";
        } else if (deviation >= 0.02 && momentum <= -0.0005) {
            signal.type = SignalType::Sell;
            signal.windowLow = ma20 * 1.015;
            signal.windowHigh = ma20 * 1.030;
            signal.message = "价格高于 20 日均线且短线开始回落";
        } else {
            signal.message = "价格尚未偏离均值区间";
        }
        return signal;
    }

private:
    std::string displayName_;
};

std::unique_ptr<TradingAlgorithm> createTradingAlgorithm(const StrategyConfig& config) {
    if (config.technicalSwing.enableTechnicalSwingStrategy) {
        StrategyConfig original = config;
        original.technicalSwing.enableTechnicalSwingStrategy = false;
        return withTechnicalSwing(createTradingAlgorithm(original), config.technicalSwing);
    }
    switch (config.kind) {
    case StrategyKind::Breakout: return std::make_unique<BreakoutAlgorithm>(config.name);
    case StrategyKind::MeanReversion: return std::make_unique<MeanReversionAlgorithm>(config.name);
    case StrategyKind::T0Intraday:
        return createT0TradingAlgorithm(config.name, defaultT0StrategyParameters());
    default: return std::make_unique<MovingAverageMomentumAlgorithm>(config.name);
    }
}

}  // namespace ashare
