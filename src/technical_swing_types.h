#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ashare {

// 与 T0StrategyParameters 一样，集中定义可由调用方覆盖的策略参数。
struct TechnicalSwingConfig {
    bool enableTechnicalSwingStrategy = false;
    bool debug = false;
    bool showChartOverlay = false;
    size_t minimumData = 120;
    std::array<size_t, 3> maPeriods{5, 10, 20};
    size_t slopeDays = 3;
    size_t macdFast = 12, macdSlow = 26, macdSignal = 9;
    size_t swingWindow = 5;
    size_t supportResistanceLookback = 120;
    size_t minimumLevelTests = 2;
    double supportResistanceTolerance = 0.015;
    double supportZoneDistance = 0.02, resistanceZoneDistance = 0.02;
    size_t volumeFast = 5, volumeSlow = 10;
    double volumeExpandRatio = 1.3, volumeContractRatio = 0.8;
    double breakoutVolumeRatio = 1.2, breakoutConfirmPct = 0.01;
    size_t breakoutConfirmDays = 2;
    double upperShadowRatio = 1.5, minimumBodyPct = 0.001;
    double rejectionPct = 0.015, stagnationPct = 0.01;
    double highOpenPct = 0.005, lowCloseFraction = 0.25;
    double supportBreakdownPct = 0.01;
    size_t noNewLowDays = 5;
    int falseBreakoutWeight = 1;
};

struct SwingPoint {
    size_t index = 0, confirmedIndex = 0;
    std::string date, confirmedDate;
    double price = 0.0;
};

// 因子结果，不能转换为自动成交指令；价格缺失用 nullopt 表示。
struct TechnicalSwingResult {
    std::string strategy = "technical_swing";
    std::string symbol, date;
    std::string technicalSignal = "INSUFFICIENT_DATA";
    std::string buySignal = "WAIT", sellSignal = "HOLD";
    std::string trend = "UNKNOWN", priceStructure = "UNKNOWN";
    int buyScore = 0, sellScore = 0;
    double buyStrength = 0.0, sellStrength = 0.0;
    double close = 0.0, ma5 = 0.0, ma10 = 0.0, ma20 = 0.0;
    double ma5Slope = 0.0, ma10Slope = 0.0, ma20Slope = 0.0;
    double dif = 0.0, dea = 0.0, macdHist = 0.0;
    double volMa5 = 0.0, volMa10 = 0.0, volumeRatio = 0.0;
    bool bearAlignment = false, strongBearTrend = false;
    bool shortRecovery = false, shortBullish = false, aboveMa20 = false, bullAlignment = false;
    bool macdGoldenCross = false, macdDeadCross = false;
    bool macdHistIncreasing = false, macdHistDecreasing = false;
    bool greenHistShrinking = false, redHistShrinking = false;
    bool difTurningUp = false, difTurningDown = false;
    bool volumeExpansion = false, volumeContraction = false;
    bool bullishVolumeConfirmation = false, sellingPressureDeclining = false;
    bool volumeStagnation = false;
    bool higherLow = false, higherHigh = false, lowerLow = false, lowerHigh = false;
    bool noNewLow = false, breakRecentSwingHigh = false, breakoutVolumeConfirmed = false;
    bool longUpperShadow = false, bearishEngulfing = false, bullishEngulfing = false;
    bool highOpenLowClose = false, rejectionFromResistance = false;
    bool nearSupport = false, nearResistance = false;
    bool breakout = false, confirmedBreakout = false, falseBreakout = false;
    bool supportRecovered = false, supportBreakdown = false, buyBlocked = false;
    std::optional<double> nearestSupport, secondSupport, nearestResistance, secondResistance;
    std::optional<double> distanceToSupportPct, distanceToResistancePct;
    std::optional<double> breakoutLevel, testedSupport;
    std::vector<SwingPoint> swingLows, swingHighs;
    std::map<std::string, bool> buyConditions, sellConditions;
    std::vector<std::string> reasons;
};

}  // namespace ashare
