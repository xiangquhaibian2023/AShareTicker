#pragma once

#include "domain.h"
#include "quote_provider.h"
#include "replay_engine.h"
#include "signal_history.h"
#include "storage.h"
#include "strategy_simulator.h"
#include "utils.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ashare {

// 后台策略线程与 UI 线程之间的单槽邮箱。
// 后台线程只写 pending 并投递窗口消息，UI 线程负责消费结果和更新控件。
struct StrategyMailbox {
    std::mutex mutex;
    std::optional<StrategyScanResult> pending;
    std::atomic_bool alive{true};
    std::atomic<HWND> window{nullptr};
};

// 策略扫描使用的历史行情缓存。后台线程通过互斥锁共享，避免高频重复请求历史 K 线。
struct StrategyHistoryEntry {
    std::vector<KLine> daily;
    std::vector<KLine> minute;
    std::chrono::steady_clock::time_point dailyUpdated{};
    int dailyRequestedLimit = 0;
    std::chrono::steady_clock::time_point minuteUpdated{};
};

struct StrategyHistoryCache {
    std::mutex mutex;
    std::map<std::string, StrategyHistoryEntry> entries;
};

// 图表中一笔模拟成交的命中区域，供鼠标悬停提示使用。
struct ChartTradeMarker {
    RECT bounds{};
    POINT anchor{};
    TradeRecord trade;
};

// 日 K 蜡烛的命中区域及其数据索引，鼠标悬停时据此显示 OHLC 详情。
struct ChartCandleMarker {
    RECT bounds{};
    POINT anchor{};
    size_t lineIndex = 0;
};

// 主窗口中正在拖动的横向分隔条。
enum class DashboardSplitter {
    None,
    ChartAndLists,
    ListAndList,
};

// 每个业务页面独立记忆当前图表标的和周期；currentSymbol/currentKlt 只是当前页镜像。
struct PageChartSelection {
    std::string symbol;
    int klt = 1;
};

// 主窗口的全部运行状态，生命周期与主窗口一致，原则上只由 UI 线程直接修改。
struct AppState {
    // Win32 窗口、控件及 GDI 资源句柄。
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND title = nullptr;
    HWND pageTitle = nullptr;
    HWND codeLabel = nullptr;
    HWND quantityLabel = nullptr;
    HWND costLabel = nullptr;
    HWND strategyLabel = nullptr;
    HWND dataModeLabel = nullptr;
    HWND dataModeCombo = nullptr;
    HWND replayDateLabel = nullptr;
    HWND replayDateEdit = nullptr;
    HWND edit = nullptr;
    HWND quantityEdit = nullptr;
    HWND costEdit = nullptr;
    HWND strategyCombo = nullptr;
    HWND holdingSummary = nullptr;
    HWND indexLabel = nullptr;
    HWND indexList = nullptr;
    HWND favoritesLabel = nullptr;
    HWND list = nullptr;
    HWND signalHistoryLabel = nullptr;
    HWND signalHistoryList = nullptr;
    HWND technicalSwingLabel = nullptr;
    HWND technicalSwingDetails = nullptr;
    HWND status = nullptr;
    HWND kline = nullptr;
    HWND screenerView = nullptr;
    HWND researchView = nullptr;
    HWND tradeAnalysisView = nullptr;
    HFONT font = nullptr;
    HFONT titleFont = nullptr;
    HFONT sectionFont = nullptr;
    HFONT iconFont = nullptr;
    HFONT chartAxisFont = nullptr;
    HFONT chartValueFont = nullptr;
    HBRUSH windowBrush = nullptr;
    HBRUSH surfaceBrush = nullptr;
    HBRUSH inputBrush = nullptr;
    HBRUSH sidebarBrush = nullptr;

    // 外部服务和本地持久化对象。数据文件统一放在程序数据目录。
    QuoteProvider provider;
    FavoriteStore store{applicationStorageDirectory() / "favorites.txt"};
    HoldingStore holdingStore{applicationStorageDirectory() / "holdings.tsv"};
    SymbolSetStore strategyStore{applicationStorageDirectory() / "strategy_symbols.txt"};
    StrategyConfigStore strategyConfigStore{applicationStorageDirectory() / "strategies.tsv",
                                            applicationStorageDirectory() / "active_strategy.txt"};
    StrategySignalStore strategySignalStore{applicationStorageDirectory() / "strategy_signals.tsv"};
    TradeRecordStore holdingTradeStore{applicationStorageDirectory() / "holding_trades.tsv"};
    TradeRecordStore strategyTradeStore{applicationStorageDirectory() / "strategy_trades.tsv"};
    AccountFundsStore accountFundsStore{applicationStorageDirectory() / "account_funds.tsv"};
    ReplayEngine replay{applicationStorageDirectory() / "replay_cache"};
    StrategySimulator strategySimulator;

    // 业务数据及行情缓存。quoteCache 避免不同页面重复请求同一标的。
    std::set<std::string> favorites;
    std::map<std::string, Holding> holdings;
    std::vector<TradeRecord> holdingTrades;
    std::vector<TradeRecord> strategyTrades;
    AccountFunds accountFunds;
    std::set<std::string> strategySymbols;
    std::vector<StrategyConfig> strategies;
    std::string activeStrategyId;
    std::map<std::string, Quote> quoteCache;
    // 名称与价格分开保存，切换回放时可以清除实时价格而继续显示证券名称。
    std::map<std::string, std::string> symbolNames;
    std::vector<TradingSignal> strategySignals;
    StrategySignalHistory strategySignalHistory;
    std::string strategySignalDate;
    // 每个交易日只在后台完整重放一次，失败时会清空并在下轮扫描重试。
    std::string strategySignalRebuildDate;
    std::set<std::string> shownAlertKeys;
    std::vector<KLine> kLines;
    std::vector<ChartTradeMarker> chartTradeMarkers;
    std::vector<ChartCandleMarker> chartCandleMarkers;
    std::optional<size_t> hoveredTradeMarker;
    std::optional<size_t> hoveredCandleMarker;
    bool trackingChartMouse = false;
    std::string currentSymbol;
    // 东方财富周期参数：1 为分钟线，101 为日线；启动默认分钟走势。
    int currentKlt = 1;
    PageChartSelection favoritesChartSelection;
    PageChartSelection holdingsChartSelection;
    PageChartSelection strategyChartSelection;
    // 独立工具页没有行情图，但保留槽位可避免跨页时覆盖其他页面选择。
    PageChartSelection screenerChartSelection;
    PageChartSelection researchChartSelection;
    PageChartSelection tradeAnalysisChartSelection;
    int refreshTicks = 0;
    DetailView detailView = DetailView::Favorites;
    MarketDataMode dataMode = MarketDataMode::Live;
    bool replayPlaying = false;
    bool updatingLists = false;
    bool strategyScanRunning = false;
    bool strategyDetailsShowTrades = false;
    // 三个页面分别记忆走势图高度；总览与策略页还分别记忆两个列表的分配比例。
    double favoritesChartRatio = 0.52;
    double holdingsChartRatio = 0.58;
    double strategyChartRatio = 0.58;
    double favoritesListRatio = 0.47;
    double strategyListRatio = 0.50;
    double holdingsListRatio = 0.52;
    DashboardSplitter activeSplitter = DashboardSplitter::None;
    std::shared_ptr<StrategyMailbox> strategyMailbox = std::make_shared<StrategyMailbox>();
    std::shared_ptr<StrategyHistoryCache> strategyHistoryCache =
        std::make_shared<StrategyHistoryCache>();
};

inline PageChartSelection& chartSelectionForView(AppState& state, DetailView view) {
    switch (view) {
    case DetailView::Favorites: return state.favoritesChartSelection;
    case DetailView::Holdings: return state.holdingsChartSelection;
    case DetailView::Strategy: return state.strategyChartSelection;
    case DetailView::Screener: return state.screenerChartSelection;
    case DetailView::Research: return state.researchChartSelection;
    case DetailView::TradeAnalysis: return state.tradeAnalysisChartSelection;
    }
    return state.favoritesChartSelection;
}

inline const PageChartSelection& chartSelectionForView(const AppState& state, DetailView view) {
    switch (view) {
    case DetailView::Favorites: return state.favoritesChartSelection;
    case DetailView::Holdings: return state.holdingsChartSelection;
    case DetailView::Strategy: return state.strategyChartSelection;
    case DetailView::Screener: return state.screenerChartSelection;
    case DetailView::Research: return state.researchChartSelection;
    case DetailView::TradeAnalysis: return state.tradeAnalysisChartSelection;
    }
    return state.favoritesChartSelection;
}

// 从主窗口的 GWLP_USERDATA 取回对应状态对象。
inline AppState* app(HWND window) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

}  // namespace ashare
