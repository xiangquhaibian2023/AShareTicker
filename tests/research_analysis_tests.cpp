#include "research_analysis.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace ashare;

    StrategyConfig strategy{"research-test", "测试算法", StrategyKind::MeanReversion};
    ResearchSecurityResult first;
    first.symbol = "sh600000";
    ResearchSecurityResult second;
    second.symbol = "sz000001";

    ResearchDayResult dayOne;
    dayOne.signalDate = "2026-08-03";
    dayOne.securities = {first};
    ResearchTradeSignalDetail buyDetail;
    buyDetail.date = dayOne.signalDate;
    buyDetail.time = "2026-08-03 10:15";
    buyDetail.symbol = first.symbol;
    buyDetail.direction = SignalType::Buy;
    buyDetail.signalPrice = 10.25;
    buyDetail.signalQuantity = 1000;
    buyDetail.executed = true;
    buyDetail.executionPrice = 10.252;
    buyDetail.executionQuantity = 1000;
    dayOne.tradeSignals = {buyDetail};
    ResearchAccountDayResult firstAccount;
    firstAccount.date = dayOne.signalDate;
    firstAccount.symbol = first.symbol;
    firstAccount.initialEquity = 10000.0;
    firstAccount.totalProfit = 100.0;
    dayOne.accounts = {firstAccount};
    dayOne.averageFollowupReturnPercent = 10.0;
    dayOne.averageSimulationReturnPercent = 2.0;

    ResearchDayResult dayTwo;
    dayTwo.signalDate = "2026-08-04";
    firstAccount.date = dayTwo.signalDate;
    firstAccount.totalProfit = 200.0;
    ResearchAccountDayResult secondAccount;
    secondAccount.date = dayTwo.signalDate;
    secondAccount.symbol = second.symbol;
    secondAccount.initialEquity = 10000.0;
    secondAccount.totalProfit = -50.0;
    dayTwo.accounts = {firstAccount, secondAccount};

    ResearchDayResult dayThree;
    dayThree.signalDate = "2026-08-05";
    dayThree.securities = {second};
    secondAccount.date = dayThree.signalDate;
    secondAccount.totalProfit = -80.0;
    dayThree.accounts = {secondAccount};
    dayThree.averageFollowupReturnPercent = -5.0;
    dayThree.averageSimulationReturnPercent = 1.0;

    std::vector<KLine> benchmark{
        {"2026-08-03", 100.0, 100.0, 101.0, 99.0, 1.0},
        {"2026-08-04", 100.0, 102.0, 103.0, 100.0, 1.0},
        {"2026-08-05", 102.0, 101.0, 103.0, 100.0, 1.0},
    };
    AlgorithmResearchReport report = buildAlgorithmResearchReport(
        strategy, "2026-08-03", "2026-08-05", "上证指数",
        {dayThree, dayOne, dayTwo}, benchmark);

    assert(report.coveredTradingDays == 3);
    assert(report.signalDays == 2);
    assert(report.securityResultCount == 2);
    assert(report.curve.size() == 3);
    assert(report.days.front().signalDate == "2026-08-03");
    assert(report.days.front().tradeSignals.size() == 1);
    assert(report.days.front().tradeSignals.front().direction == SignalType::Buy);
    assert(report.days.front().tradeSignals.front().signalQuantity == 1000);
    assert(std::abs(report.signalCumulativePercent - 4.5) < 1e-9);
    assert(std::abs(report.simulationCumulativePercent - 3.02) < 1e-9);
    assert(std::abs(report.benchmarkCumulativePercent - 1.0) < 1e-9);
    assert(std::abs(report.signalExcessPercent - 3.5) < 1e-9);
    assert(std::abs(report.simulationExcessPercent - 2.02) < 1e-9);
    assert(std::abs(report.simulationTotalProfit - 120.0) < 1e-9);
    assert(std::abs(report.simulationInitialCapital - 20000.0) < 1e-9);

    std::cout << "Research analysis tests passed\n";
    return 0;
}
