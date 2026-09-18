#pragma once

#include <algorithm>
#include <memory>
#include "technical_swing_types.h"
#include <string>
#include <vector>

namespace ashare {

// 单个证券的实时快照。价格和成交字段均使用行情源原始单位。
struct Quote {
    std::string symbol;
    std::string name;
    double price = 0.0;
    double previousClose = 0.0;
    double open = 0.0;
    double change = 0.0;
    double changePercent = 0.0;
    double volume = 0.0;
    double amount = 0.0;
    std::string time;
    // 选股评分使用当日最高、最低和换手率；追加字段可保持旧聚合初始化代码兼容。
    double high = 0.0;
    double low = 0.0;
    double turnoverRate = 0.0;
};

// 一根 K 线。分钟线和日线共用该结构，通过 date 的格式区分周期。
struct KLine {
    std::string date;
    double open = 0.0;
    double close = 0.0;
    double high = 0.0;
    double low = 0.0;
    double volume = 0.0;
};

// 分钟数据在线性交易时间轴上的位置：午休不占用横轴宽度。
struct TradingMinutePosition {
    int minute = 0;
    bool afternoon = false;
};

// 持仓及当日成交汇总。成本价表示当前剩余持仓的单位成本。
struct Holding {
    std::string symbol;
    long long quantity = 0;
    double cost = 0.0;
    std::string todayTradeDate;
    long long todayBuyQuantity = 0;
    double todayBuyPrice = 0.0;
    long long todaySellQuantity = 0;
    double todaySellPrice = 0.0;
    double todayFees = 0.0;
    // 多轮 T0 的状态不能仅靠全天成交均价恢复，因此单独保存轮次和当前未闭合腿价格。
    int todayT0CompletedRounds = 0;
    double todayT0OpenPrice = 0.0;
    double todayT0LastClosePrice = 0.0;
    // 当天第一笔分笔成交前的持仓基线；修改或删除成交时从该基线完整重算。
    std::string tradeBaseDate;
    long long tradeBaseQuantity = 0;
    double tradeBaseCost = 0.0;
};

// 当日成交只在 todayTradeDate 对应的交易日参与盈亏计算。
inline void clearIntradayTrades(Holding& holding) {
    holding.todayTradeDate.clear();
    holding.todayBuyQuantity = 0;
    holding.todayBuyPrice = 0.0;
    holding.todaySellQuantity = 0;
    holding.todaySellPrice = 0.0;
    holding.todayFees = 0.0;
    holding.todayT0CompletedRounds = 0;
    holding.todayT0OpenPrice = 0.0;
    holding.todayT0LastClosePrice = 0.0;
}

// 已闭合的当日 T0 净盈亏。成交量不完全匹配时按已闭合名义金额分摊录入费用。
inline double closedT0NetProfit(const Holding& holding, const std::string& dateToken) {
    if (holding.todayTradeDate != dateToken) {
        return 0.0;
    }
    long long matchedQuantity = std::min(holding.todayBuyQuantity,
                                         holding.todaySellQuantity);
    if (matchedQuantity <= 0 || holding.todayBuyPrice <= 0.0 ||
        holding.todaySellPrice <= 0.0) {
        return 0.0;
    }
    double grossProfit = (holding.todaySellPrice - holding.todayBuyPrice) *
                         static_cast<double>(matchedQuantity);
    double totalNotional = holding.todayBuyPrice * static_cast<double>(holding.todayBuyQuantity) +
                           holding.todaySellPrice * static_cast<double>(holding.todaySellQuantity);
    double matchedNotional = (holding.todayBuyPrice + holding.todaySellPrice) *
                             static_cast<double>(matchedQuantity);
    double allocatedFees = totalNotional > 0.0
                               ? holding.todayFees * std::min(1.0, matchedNotional / totalNotional)
                               : 0.0;
    return grossProfit - allocatedFees;
}

enum class DetailView {
    Favorites,
    Holdings,
    Strategy,
    Screener,
    Research,
    TradeAnalysis,
};

// 行情工作模式。回放模式只消费指定交易日的历史数据，不混入实时价格。
enum class MarketDataMode {
    Live,
    Replay,
};

// 算法给出的方向性信号；None 表示继续观察。
enum class SignalType {
    None,
    Buy,
    Sell,
};

enum class TradeSource {
    Manual,
    StrategySimulation,
};

// 一笔真实或策略模拟成交。各项费用独立保存，避免汇总均价掩盖分笔差异。
struct TradeRecord {
    std::string id;
    std::string strategyId;
    std::string symbol;
    std::string time;
    SignalType side = SignalType::None;
    long long quantity = 0;
    double price = 0.0;
    double commission = 0.0;
    double stampDuty = 0.0;
    double transferFee = 0.0;
    double otherFees = 0.0;
    TradeSource source = TradeSource::Manual;
};

inline double tradeFees(const TradeRecord& trade) {
    return trade.commission + trade.stampDuty + trade.transferFee + trade.otherFees;
}

struct AccountFunds {
    double totalAssets = 0.0;
    double availableCash = 0.0;
    std::string updatedAt;
};

// 内置策略模板类型，新增算法时应同步更新序列化与工厂函数。
enum class StrategyKind {
    MovingAverageMomentum,
    Breakout,
    MeanReversion,
    T0Intraday,
};

// 用户可管理的策略配置。id 用于持久化，name 仅用于界面展示。
struct StrategyConfig {
    std::string id;
    std::string name;
    StrategyKind kind = StrategyKind::MovingAverageMomentum;
    TechnicalSwingConfig technicalSwing{};
};

// 单个证券的一次策略评估结果。
struct TradingSignal {
    std::string symbol;
    std::string name;
    std::string algorithm;
    SignalType type = SignalType::None;
    double currentPrice = 0.0;
    double windowLow = 0.0;
    double windowHigh = 0.0;
    std::string message;
    std::string time;
    // T0 策略会填写建议回转数量和成本收益估算，其他策略保持默认值。
    long long suggestedQuantity = 0;
    double estimatedCost = 0.0;
    double expectedGrossProfit = 0.0;
    double expectedNetProfit = 0.0;
    std::shared_ptr<const TechnicalSwingResult> technicalSwingResult{};
};

// 后台扫描线程传回 UI 线程的批量结果。
struct StrategyScanResult {
    std::vector<TradingSignal> signals;
    // 客户端重启后由当日真实分钟行情重放得到的信号转换序列。
    std::vector<TradingSignal> reconstructedSignals;
    std::vector<TradeRecord> reconstructedTrades;
    std::string error;
    std::string reconstructionError;
    std::string strategyId;
    bool reconstructionAttempted = false;
};

}  // namespace ashare
