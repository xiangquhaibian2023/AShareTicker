#include "broker_statement.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace ashare;

int main(int argc, char** argv) {
    if (argc > 1) {
        BrokerStatement imported = loadBrokerStatementCsv(argv[1]);
        std::cout << "trades=" << imported.trades.size()
                  << " positions=" << imported.positions.size()
                  << " issues=" << imported.issues.size() << '\n';
        BrokerStatementAnalysis importedAnalysis = analyzeBrokerStatement(imported);
        std::cout << "buyNotional=" << importedAnalysis.buyNotional
                  << " sellNotional=" << importedAnalysis.sellNotional
                  << " fees=" << importedAnalysis.totalFees
                  << " netCashFlow=" << importedAnalysis.netCashFlow
                  << " matchedProfit=" << importedAnalysis.matchedProfit << '\n';
        for (const auto& issue : imported.issues) std::cout << issue << '\n';
        return imported.trades.empty() ? 1 : 0;
    }
    const std::string csv =
        "发生日期,买卖类别,证券代码,证券名称,成交数量,成交价格,总发生金额,手续费,印花税,过户费,资金余额\n"
        "20260916,证券卖出,2594,比亚迪,100,110,10994,5,1,0,20000\n"
        "20260915,证券买入,2594,比亚迪,100,100,-10005,5,0,0,9000\n"
        ",,,,,,,,,,\n"
        "交易市场,证券代码,证券名称,持仓数量,市价,成本价,证券市值,,,,\n"
        "沪市 A 股,513050,中概互联网ETF,50000,1.015,1.215,50750,,,,\n"
        "证券市值 (RMB):,50750,证券市值 (HKD):,0,,,,,,,\n"
        "总资产 (RMB):,70000,总资产 (HKD):,0,,,,,,,\n"
        "资金余额 (RMB):,10,资金余额 (HKD):,0,,,,,,,\n"
        "资金可用 (RMB):,19250,资金可用 (HKD):,0,,,,,,,\n";
    BrokerStatement statement = parseBrokerStatementCsv(csv);
    assert(statement.trades.size() == 2);
    assert(statement.positions.size() == 1);
    assert(statement.trades[0].symbol == "sz002594");
    assert(statement.positions[0].symbol == "sh513050");
    assert(statement.totalAssets == 70000.0);
    assert(statement.availableCash == 19250.0);
    assert(statement.cashBalance == 10.0);
    assert(statement.issues.empty());

    BrokerStatementAnalysis analysis = analyzeBrokerStatement(statement);
    assert(analysis.tradeCount == 2);
    assert(analysis.symbols.size() == 1);
    assert(analysis.symbols[0].matchedQuantity == 100);
    assert(std::abs(analysis.symbols[0].matchedProfit - 989.0) < 0.001);
    std::cout << "Broker statement tests passed\n";
    return 0;
}
