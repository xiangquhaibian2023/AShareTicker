#pragma once

#include "domain.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ashare {

struct T0CostEstimate {
    double buyCommission = 0.0;
    double sellCommission = 0.0;
    double stampDuty = 0.0;
    double transferFee = 0.0;
    double slippage = 0.0;
    double effectiveSlippageRatePerSide = 0.0;
    double total = 0.0;
};

struct T0TransactionCostParameters {
    double commissionRate = 0.0001;
    double minimumCommission = 5.0;
    double stockStampDutyRate = 0.0005;
    double stockTransferFeeRate = 0.00001;
    double slippageRatePerSide = 0.0002;
    double volatilitySlippageMultiplier = 0.02;
    double orderParticipationSlippageMultiplier = 0.0025;
    double maximumSlippageRatePerSide = 0.0015;
};

struct T0CostContext {
    double relativeVolatility = 0.0;
    double marketAmount = 0.0;
};

// T0 的行情、状态、委托和费用阈值统一收敛于此，回测工具可直接构造候选参数。
struct T0StrategyParameters {
    size_t sampleCount = 25;
    size_t minimumSampleCount = 6;
    size_t shortMomentumLookback = 5;
    // 均值回归不再只检查相邻三根 K 线，而是在该窗口内寻找极值后的恢复确认。
    size_t meanReversionLookback = 8;
    // 极值观察窗口与转向确认窗口分离，避免低价 ETF 因同价平台漏掉拐点。
    size_t reversalConfirmationLookback = 3;
    int minimumReversalBarsAfterExtreme = 2;
    size_t dailyFastPeriod = 5;
    size_t dailySlowPeriod = 20;
    double vwapCenterWeight = 0.5;
    // 近期低波动 ETF 的中心偏离多数低于 0.55%，降低基线但仍由波动率动态上调。
    double minimumDeviation = 0.0030;
    double deviationVolatilityMultiplier = 0.65;
    double minimumReversalRecoveryRate = 0.0005;
    int minimumStockReversalTicks = 1;
    int minimumFundReversalTicks = 2;
    // 反转出现后仍限制中窗斜率，防止在尚未减速的单边行情中抄底或摸顶。
    double maximumMeanCounterSlopePerMinute = 0.00070;
    double minimumTrendThreshold = 0.0045;
    double trendVolatilityMultiplier = 1.5;
    double trendDirectionRatio = 0.58;
    double shortMomentumThreshold = 0.0010;
    double minimumRegressionSlopePerMinute = 0.00006;
    double regressionSlopeVolatilityMultiplier = 0.50;
    double minimumRegressionFit = 0.70;
    size_t trendRecentSlopeLookback = 8;
    double minimumRecentToLongSlopeRatio = 0.25;
    double trendProfitVolatilityMultiplier = 0.8;
    double minimumTrendProfitRate = 0.0035;
    double maximumTrendProfitRate = 0.008;
    double meanProfitMultiplier = 0.8;
    // 覆盖双倍成本且净收益不少于 10 元后允许快速止盈，降低已获利敞口回撤风险。
    double minimumMeanProfitRate = 0.0020;
    double maximumMeanProfitRate = 0.010;
    double stopLossRate = 0.0040;
    double roundPositionRate = 0.40;
    // 为满足最低有效成交额可动态提高单轮数量，但任何一轮不超过日初底仓的一半。
    double maximumRoundPositionRate = 0.50;
    double reentryMoveRate = 0.0050;
    // 预期毛差至少覆盖完整双边成本两倍，仍保留一倍成本作为滑点和延迟缓冲。
    double costSafetyMultiple = 2.0;
    double minimumNetProfit = 10.0;
    double minimumOrderValue = 5000.0;
    double minimumLiquidityAmount = 20000000.0;
    double maximumDailyTrendDeviation = 0.05;
    // 非趋势信号不得逆着已经形成的盘中单边方向开腿。
    double sessionTrendGuardRate = 0.0100;
    double minimumLimitWindowRate = 0.0005;
    double maximumLimitWindowRate = 0.0030;
    double limitWindowVolatilityMultiplier = 0.35;
    double trendLimitWindowMultiplier = 1.25;
    double standardPriceLimitRate = 0.10;
    double riskWarningPriceLimitRate = 0.10;
    double growthBoardPriceLimitRate = 0.20;
    double beijingPriceLimitRate = 0.30;
    double priceLimitEmergencyBufferRate = 0.001;
    double stockPriceTick = 0.01;
    double fundPriceTick = 0.001;
    double externalCostChangeTolerance = 0.0005;
    long long lotSize = 100;
    int maximumRoundsPerDay = 3;
    int entryStartMinute = 15;
    int middayEntryBlockStartMinute = 115;
    int middayEntryBlockEndMinute = 125;
    // 策略连续分钟坐标中 180=14:00、220=14:40；此后只管理已有敞口。
    int entryEndMinute = 180;
    int forceCloseMinute = 220;
    T0TransactionCostParameters transactionCosts;
};

// 策略扩展接口。算法只读取行情并返回信号，不直接操作 UI 或持仓。
class TradingAlgorithm {
public:
    virtual ~TradingAlgorithm() = default;
    virtual std::string name() const = 0;
    virtual TradingSignal evaluate(const Quote& quote, const std::vector<KLine>& daily,
                                   const std::vector<KLine>& minute,
                                   const Holding* holding = nullptr) const = 0;
};

// 股票成本包含双边佣金、卖出印花税、双边过户费和滑点预留；ETF 不计后两项税费。
T0CostEstimate estimateT0RoundTripCost(double buyPrice, double sellPrice, long long quantity,
                                       bool fundLike);
T0CostEstimate estimateT0RoundTripCost(double buyPrice, double sellPrice, long long quantity,
                                       bool fundLike,
                                       const T0TransactionCostParameters& parameters);
T0CostEstimate estimateT0RoundTripCost(double buyPrice, double sellPrice, long long quantity,
                                       bool fundLike,
                                       const T0TransactionCostParameters& parameters,
                                       const T0CostContext& context);

// StrategyKind 与持久化字符串之间的稳定映射。
std::string strategyKindKey(StrategyKind kind);
std::optional<StrategyKind> strategyKindFromKey(const std::string& key);
std::vector<StrategyConfig> defaultStrategies();
const T0StrategyParameters& defaultT0StrategyParameters();
std::unique_ptr<TradingAlgorithm> createT0TradingAlgorithm(
    const std::string& displayName, const T0StrategyParameters& parameters);
// 根据配置创建算法实例；新增策略实现时在该工厂注册。
std::unique_ptr<TradingAlgorithm> createTradingAlgorithm(const StrategyConfig& config);

}  // namespace ashare
