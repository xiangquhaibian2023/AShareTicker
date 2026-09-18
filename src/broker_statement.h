#pragma once

#include "domain.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ashare {

// 券商交割单中的一笔历史成交。保留发生金额和资金余额，便于与原始报表核对。
struct BrokerTradeRecord {
    std::string date;
    SignalType side = SignalType::None;
    std::string symbol;
    std::string name;
    long long quantity = 0;
    double price = 0.0;
    double amount = 0.0;
    double commission = 0.0;
    double stampDuty = 0.0;
    double transferFee = 0.0;
    double balance = 0.0;
};

// 券商报表尾部的持仓快照，与程序手工维护的持仓分开保存，只有用户确认后才同步。
struct BrokerPositionSnapshot {
    std::string market;
    std::string symbol;
    std::string name;
    long long quantity = 0;
    double marketPrice = 0.0;
    double cost = 0.0;
    double marketValue = 0.0;
};

struct BrokerStatement {
    std::vector<BrokerTradeRecord> trades;
    std::vector<BrokerPositionSnapshot> positions;
    double securitiesMarketValue = 0.0;
    double totalAssets = 0.0;
    double availableCash = 0.0;
    double cashBalance = 0.0;
    std::string firstTradeDate;
    std::string lastTradeDate;
    std::vector<std::string> issues;
};

struct BrokerSymbolSummary {
    std::string symbol;
    std::string name;
    int tradeCount = 0;
    long long buyQuantity = 0;
    long long sellQuantity = 0;
    long long netQuantity = 0;
    long long matchedQuantity = 0;
    long long unmatchedSellQuantity = 0;
    double averageBuyPrice = 0.0;
    double averageSellPrice = 0.0;
    double totalFees = 0.0;
    double netCashFlow = 0.0;
    double matchedProfit = 0.0;
    double matchedReturnPercent = 0.0;
};

struct BrokerStatementAnalysis {
    int tradeCount = 0;
    int buyCount = 0;
    int sellCount = 0;
    double buyNotional = 0.0;
    double sellNotional = 0.0;
    double totalFees = 0.0;
    double netCashFlow = 0.0;
    double matchedProfit = 0.0;
    std::vector<BrokerSymbolSummary> symbols;
};

// CSV 使用 RFC 4180 引号规则，并按“成交/持仓/账户汇总”三个表头分段解析。
BrokerStatement parseBrokerStatementCsv(const std::string& content);
BrokerStatement loadBrokerStatementCsv(const std::filesystem::path& path);
BrokerStatement importBrokerStatementCsv(const std::filesystem::path& source,
                                         const std::filesystem::path& destination);
BrokerStatementAnalysis analyzeBrokerStatement(const BrokerStatement& statement);

}  // namespace ashare
