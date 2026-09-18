#include "storage.h"

#include "trading_algorithm.h"
#include "technical_swing_strategy.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ashare {
namespace {

std::string tsvField(std::string value) {
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

}  // namespace

FavoriteStore::FavoriteStore(std::filesystem::path path) : path_(std::move(path)) {}

std::set<std::string> FavoriteStore::load() const {
    std::set<std::string> symbols;
    std::ifstream input(path_);
    std::string line;
    while (std::getline(input, line)) {
        if (auto symbol = normalizeSymbol(line)) symbols.insert(*symbol);
    }
    return symbols;
}

void FavoriteStore::save(const std::set<std::string>& symbols) const {
    std::ostringstream output;
    for (const auto& symbol : symbols) output << symbol << '\n';
    writeTextFileAtomically(path_, output.str());
}

HoldingStore::HoldingStore(std::filesystem::path path) : path_(std::move(path)) {}

std::map<std::string, Holding> HoldingStore::load() const {
    std::map<std::string, Holding> holdings;
    std::ifstream input(path_);
    std::string line;
    while (std::getline(input, line)) {
        auto fields = split(line, '\t');
        // 兼容历史格式；15 列版本增加分笔成交重算所需的日初基线。
        if (fields.size() != 3 && fields.size() != 8 && fields.size() != 9 &&
            fields.size() != 12 && fields.size() != 15) continue;
        auto symbol = normalizeSymbol(fields[0]);
        try {
            long long quantity = std::stoll(fields[1]);
            double cost = std::stod(fields[2]);
            if (!symbol || quantity <= 0 || !std::isfinite(cost) || cost <= 0.0) continue;
            Holding holding;
            holding.symbol = *symbol;
            holding.quantity = quantity;
            // 持仓成本统一按三位小数保存和参与计算。
            holding.cost = std::round(cost * 1000.0) / 1000.0;
            try {
                size_t offset = 3;
                if (fields.size() == 9 || fields.size() == 12 || fields.size() == 15) {
                    holding.todayTradeDate = fields[3];
                    offset = 4;
                }
                if (fields.size() >= 8) {
                    holding.todayBuyQuantity = std::stoll(fields[offset]);
                    holding.todayBuyPrice = std::stod(fields[offset + 1]);
                    holding.todaySellQuantity = std::stoll(fields[offset + 2]);
                    holding.todaySellPrice = std::stod(fields[offset + 3]);
                    holding.todayFees = std::stod(fields[offset + 4]);
                }
                if (fields.size() == 12 || fields.size() == 15) {
                    holding.todayT0CompletedRounds = std::stoi(fields[9]);
                    holding.todayT0OpenPrice = std::stod(fields[10]);
                    holding.todayT0LastClosePrice = std::stod(fields[11]);
                }
                if (fields.size() == 15) {
                    holding.tradeBaseDate = fields[12];
                    holding.tradeBaseQuantity = std::stoll(fields[13]);
                    holding.tradeBaseCost = std::stod(fields[14]);
                }
            } catch (const std::exception&) {
                clearIntradayTrades(holding);
            }
            // 当前数量 = 日初数量 + 买入 - 卖出，反推日初数量用于校验成交汇总。
            long long opening = holding.quantity - holding.todayBuyQuantity + holding.todaySellQuantity;
            bool valid = holding.todayBuyQuantity >= 0 && holding.todaySellQuantity >= 0 &&
                         std::isfinite(holding.todayBuyPrice) && std::isfinite(holding.todaySellPrice) &&
                         std::isfinite(holding.todayFees) && holding.todayBuyPrice >= 0.0 &&
                         holding.todaySellPrice >= 0.0 && holding.todayFees >= 0.0 && opening >= 0 &&
                         holding.todayT0CompletedRounds >= 0 &&
                         std::isfinite(holding.todayT0OpenPrice) &&
                         std::isfinite(holding.todayT0LastClosePrice) &&
                         holding.todayT0OpenPrice >= 0.0 && holding.todayT0LastClosePrice >= 0.0 &&
                         holding.tradeBaseQuantity >= 0 &&
                         std::isfinite(holding.tradeBaseCost) && holding.tradeBaseCost >= 0.0 &&
                         (holding.todayBuyQuantity == 0 || holding.todayBuyPrice > 0.0) &&
                         (holding.todaySellQuantity == 0 || holding.todaySellPrice > 0.0);
            // 当日成交跨交易日自动失效，防止旧现金流进入今天的盈亏。
            if (!valid || holding.todayTradeDate != currentDateToken()) clearIntradayTrades(holding);
            holdings[*symbol] = holding;
        } catch (const std::exception&) {
        }
    }
    return holdings;
}

void HoldingStore::save(const std::map<std::string, Holding>& holdings) const {
    std::ostringstream output;
    // 使用足够精度写入，界面显示精度与磁盘保存精度相互独立。
    output << std::setprecision(12);
    for (const auto& [symbol, holding] : holdings) {
        output << symbol << '\t' << holding.quantity << '\t' << holding.cost << '\t'
               << holding.todayTradeDate << '\t' << holding.todayBuyQuantity << '\t'
               << holding.todayBuyPrice << '\t' << holding.todaySellQuantity << '\t'
               << holding.todaySellPrice << '\t' << holding.todayFees << '\t'
               << holding.todayT0CompletedRounds << '\t' << holding.todayT0OpenPrice << '\t'
               << holding.todayT0LastClosePrice << '\t' << holding.tradeBaseDate << '\t'
               << holding.tradeBaseQuantity << '\t' << holding.tradeBaseCost << '\n';
    }
    writeTextFileAtomically(path_, output.str());
}

TradeRecordStore::TradeRecordStore(std::filesystem::path path) : path_(std::move(path)) {}

std::vector<TradeRecord> TradeRecordStore::load() const {
    std::vector<TradeRecord> records;
    std::ifstream input(path_);
    std::string line;
    while (std::getline(input, line)) {
        auto fields = split(line, '\t');
        if (fields.size() != 13) continue;
        try {
            auto symbol = normalizeSymbol(fields[2]);
            int side = std::stoi(fields[4]);
            int source = std::stoi(fields[12]);
            TradeRecord record;
            record.id = fields[0];
            record.strategyId = fields[1];
            record.symbol = symbol ? *symbol : std::string{};
            record.time = fields[3];
            record.side = static_cast<SignalType>(side);
            record.quantity = std::stoll(fields[5]);
            record.price = std::stod(fields[6]);
            record.commission = std::stod(fields[7]);
            record.stampDuty = std::stod(fields[8]);
            record.transferFee = std::stod(fields[9]);
            record.otherFees = std::stod(fields[10]);
            std::string savedDate = fields[11];
            record.source = static_cast<TradeSource>(source);
            bool valid = !record.id.empty() && symbol && savedDate.size() == 8 &&
                         (side == static_cast<int>(SignalType::Buy) ||
                          side == static_cast<int>(SignalType::Sell)) &&
                         (source == static_cast<int>(TradeSource::Manual) ||
                          source == static_cast<int>(TradeSource::StrategySimulation)) &&
                         record.quantity > 0 && std::isfinite(record.price) && record.price > 0.0 &&
                         std::isfinite(record.commission) && record.commission >= 0.0 &&
                         std::isfinite(record.stampDuty) && record.stampDuty >= 0.0 &&
                         std::isfinite(record.transferFee) && record.transferFee >= 0.0 &&
                         std::isfinite(record.otherFees) && record.otherFees >= 0.0;
            std::string actualDate;
            for (char value : record.time) {
                if (value >= '0' && value <= '9') actualDate.push_back(value);
                if (actualDate.size() == 8) break;
            }
            if (valid && actualDate == savedDate) records.push_back(std::move(record));
        } catch (const std::exception&) {
        }
    }
    return records;
}

void TradeRecordStore::save(const std::vector<TradeRecord>& records) const {
    std::ostringstream output;
    output << std::setprecision(17);
    for (const auto& record : records) {
        std::string date;
        for (char value : record.time) {
            if (value >= '0' && value <= '9') date.push_back(value);
            if (date.size() == 8) break;
        }
        if (record.id.empty() || date.size() != 8 || record.side == SignalType::None) continue;
        output << tsvField(record.id) << '\t' << tsvField(record.strategyId) << '\t'
               << tsvField(record.symbol) << '\t' << tsvField(record.time) << '\t'
               << static_cast<int>(record.side) << '\t' << record.quantity << '\t'
               << record.price << '\t' << record.commission << '\t' << record.stampDuty << '\t'
               << record.transferFee << '\t' << record.otherFees << '\t' << date << '\t'
               << static_cast<int>(record.source) << '\n';
    }
    writeTextFileAtomically(path_, output.str());
}

AccountFundsStore::AccountFundsStore(std::filesystem::path path) : path_(std::move(path)) {}

AccountFunds AccountFundsStore::load() const {
    AccountFunds result;
    std::ifstream input(path_);
    std::string line;
    if (!std::getline(input, line)) return result;
    auto fields = split(line, '\t');
    if (fields.size() != 3) return result;
    try {
        result.totalAssets = std::stod(fields[0]);
        result.availableCash = std::stod(fields[1]);
        result.updatedAt = fields[2];
        if (!std::isfinite(result.totalAssets) || !std::isfinite(result.availableCash) ||
            result.totalAssets < 0.0 || result.availableCash < 0.0 ||
            result.availableCash > result.totalAssets) {
            return {};
        }
    } catch (const std::exception&) {
        return {};
    }
    return result;
}

void AccountFundsStore::save(const AccountFunds& funds) const {
    std::ostringstream output;
    output << std::setprecision(17) << funds.totalAssets << '\t' << funds.availableCash << '\t'
           << tsvField(funds.updatedAt) << '\n';
    writeTextFileAtomically(path_, output.str());
}

SymbolSetStore::SymbolSetStore(std::filesystem::path path) : path_(std::move(path)) {}

std::set<std::string> SymbolSetStore::load() const {
    std::set<std::string> symbols;
    std::ifstream input(path_);
    std::string line;
    while (std::getline(input, line)) {
        if (auto symbol = normalizeSymbol(line)) symbols.insert(*symbol);
    }
    return symbols;
}

void SymbolSetStore::save(const std::set<std::string>& symbols) const {
    std::ostringstream output;
    for (const auto& symbol : symbols) output << symbol << '\n';
    writeTextFileAtomically(path_, output.str());
}

StrategyConfigStore::StrategyConfigStore(std::filesystem::path configsPath,
                                         std::filesystem::path activePath)
    : configsPath_(std::move(configsPath)), activePath_(std::move(activePath)) {}

std::vector<StrategyConfig> StrategyConfigStore::load() const {
    std::vector<StrategyConfig> configs;
    std::ifstream input(configsPath_);
    std::string line;
    while (std::getline(input, line)) {
        auto fields = split(line, '\t');
        if (fields.size() != 3 && fields.size() != 4) continue;
        auto kind = strategyKindFromKey(fields[2]);
        std::string id = trim(fields[0]);
        std::string name = trim(fields[1]);
        if (kind && !id.empty() && !name.empty()) {
            StrategyConfig config{id, name, *kind};
            if (fields.size() == 4 && !parseTechnicalSwingConfig(fields[3], config.technicalSwing)) {
                logger().write("WARN", "Invalid technical swing configuration: " + id);
            }
            configs.push_back(std::move(config));
        }
    }
    // 首次启动或配置损坏时恢复内置策略，保证组合框始终可用。
    if (configs.empty()) {
        return defaultStrategies();
    }
    // 兼容旧版配置：升级后自动补入内置 T0 策略。
    bool hasT0 = std::any_of(configs.begin(), configs.end(), [](const StrategyConfig& config) {
        return config.kind == StrategyKind::T0Intraday;
    });
    if (!hasT0) {
        configs.push_back({"builtin_t0_intraday", "T0 日内回转", StrategyKind::T0Intraday});
    }
    return configs;
}

void StrategyConfigStore::save(const std::vector<StrategyConfig>& configs) const {
    std::ostringstream output;
    for (const auto& config : configs) {
        output << config.id << '\t' << config.name << '\t' << strategyKindKey(config.kind)
               << '\t' << serializeTechnicalSwingConfig(config.technicalSwing) << '\n';
    }
    writeTextFileAtomically(configsPath_, output.str());
}

std::string StrategyConfigStore::loadActive() const {
    std::ifstream input(activePath_);
    std::string id;
    std::getline(input, id);
    return trim(id);
}

void StrategyConfigStore::saveActive(const std::string& id) const {
    writeTextFileAtomically(activePath_, id + "\n");
}

StrategySignalStore::StrategySignalStore(std::filesystem::path path)
    : path_(std::move(path)) {}

std::vector<StrategySignalRecord> StrategySignalStore::load(
    const std::string& dateToken) const {
    std::vector<StrategySignalRecord> records;
    std::ifstream input(path_);
    std::string line;
    while (std::getline(input, line)) {
        auto fields = split(line, '\t');
        if (fields.size() != 16 || fields[0] != dateToken || fields[1] != "live") {
            continue;
        }
        try {
            auto symbol = normalizeSymbol(fields[3]);
            int typeValue = std::stoi(fields[6]);
            if (!symbol || fields[2].empty() ||
                (typeValue != static_cast<int>(SignalType::Buy) &&
                 typeValue != static_cast<int>(SignalType::Sell))) {
                continue;
            }
            StrategySignalRecord record;
            record.dataMode = MarketDataMode::Live;
            record.strategyId = fields[2];
            record.signal.symbol = *symbol;
            record.signal.name = fields[4];
            record.signal.algorithm = fields[5];
            record.signal.type = static_cast<SignalType>(typeValue);
            record.signal.currentPrice = std::stod(fields[7]);
            record.signal.windowLow = std::stod(fields[8]);
            record.signal.windowHigh = std::stod(fields[9]);
            record.signal.message = fields[10];
            record.signal.time = fields[11];
            record.signal.suggestedQuantity = std::stoll(fields[12]);
            record.signal.estimatedCost = std::stod(fields[13]);
            record.signal.expectedGrossProfit = std::stod(fields[14]);
            record.signal.expectedNetProfit = std::stod(fields[15]);
            bool valid = std::isfinite(record.signal.currentPrice) &&
                         std::isfinite(record.signal.windowLow) &&
                         std::isfinite(record.signal.windowHigh) &&
                         std::isfinite(record.signal.estimatedCost) &&
                         std::isfinite(record.signal.expectedGrossProfit) &&
                         std::isfinite(record.signal.expectedNetProfit) &&
                         record.signal.currentPrice > 0.0 &&
                         record.signal.windowLow >= 0.0 && record.signal.windowHigh >= 0.0 &&
                         record.signal.suggestedQuantity >= 0;
            if (valid) {
                records.push_back(std::move(record));
            }
        } catch (const std::exception&) {
        }
    }
    return records;
}

void StrategySignalStore::save(const std::string& dateToken,
                               const std::vector<StrategySignalRecord>& records) const {
    std::ostringstream output;
    output << std::setprecision(17);
    for (const auto& record : records) {
        if (record.dataMode != MarketDataMode::Live ||
            record.signal.type == SignalType::None) {
            continue;
        }
        const TradingSignal& signal = record.signal;
        output << dateToken << '\t' << "live" << '\t' << tsvField(record.strategyId) << '\t'
               << tsvField(signal.symbol) << '\t' << tsvField(signal.name) << '\t'
               << tsvField(signal.algorithm) << '\t' << static_cast<int>(signal.type) << '\t'
               << signal.currentPrice << '\t' << signal.windowLow << '\t' << signal.windowHigh
               << '\t' << tsvField(signal.message) << '\t' << tsvField(signal.time) << '\t'
               << signal.suggestedQuantity << '\t' << signal.estimatedCost << '\t'
               << signal.expectedGrossProfit << '\t' << signal.expectedNetProfit << '\n';
    }
    writeTextFileAtomically(path_, output.str());
}

}  // namespace ashare
