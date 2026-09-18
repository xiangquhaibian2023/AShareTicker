#include "broker_statement.h"

#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace ashare {
namespace {

std::string trimCell(std::string value) {
    auto whitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::vector<std::string> parseCsvRow(const std::string& row) {
    std::vector<std::string> cells;
    std::string cell;
    bool quoted = false;
    for (size_t index = 0; index < row.size(); ++index) {
        char ch = row[index];
        if (ch == '"') {
            if (quoted && index + 1 < row.size() && row[index + 1] == '"') {
                cell.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (ch == ',' && !quoted) {
            cells.push_back(trimCell(cell));
            cell.clear();
        } else if (ch != '\r') {
            cell.push_back(ch);
        }
    }
    cells.push_back(trimCell(cell));
    return cells;
}

bool allDigits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

std::string normalizeBrokerSymbol(std::string code) {
    code = trimCell(std::move(code));
    if (!allDigits(code) || code.size() > 6) return {};
    code.insert(code.begin(), 6 - code.size(), '0');
    char first = code.front();
    if (first == '5' || first == '6' || first == '9') return "sh" + code;
    if (first == '0' || first == '1' || first == '2' || first == '3') return "sz" + code;
    return {};
}

bool parseDouble(const std::string& text, double& value) {
    try {
        size_t consumed = 0;
        value = std::stod(trimCell(text), &consumed);
        return consumed == trimCell(text).size() && std::isfinite(value);
    } catch (...) {
        return false;
    }
}

bool parseQuantity(const std::string& text, long long& value) {
    try {
        std::string normalized = trimCell(text);
        size_t consumed = 0;
        value = std::stoll(normalized, &consumed);
        return consumed == normalized.size() && value >= 0;
    } catch (...) {
        return false;
    }
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("无法打开券商 CSV 文件。");
    std::ostringstream content;
    content << input.rdbuf();
    if (!input.good() && !input.eof()) throw std::runtime_error("读取券商 CSV 文件失败。");
    return content.str();
}

bool blankRow(const std::vector<std::string>& cells) {
    return std::all_of(cells.begin(), cells.end(), [](const std::string& cell) {
        return cell.empty();
    });
}

std::string displayCode(const std::string& symbol) {
    return symbol.size() == 8 ? symbol.substr(2) : symbol;
}

}  // namespace

BrokerStatement parseBrokerStatementCsv(const std::string& rawContent) {
    BrokerStatement result;
    std::string content = rawContent;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    enum class Section { None, Trades, Positions };
    Section section = Section::None;
    std::istringstream stream(content);
    std::string line;
    size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        auto cells = parseCsvRow(line);
        if (cells.empty() || blankRow(cells)) continue;
        if (cells[0] == "发生日期") {
            section = Section::Trades;
            continue;
        }
        if (cells[0] == "交易市场") {
            section = Section::Positions;
            continue;
        }
        if (cells[0] == "证券市值 (RMB):" || cells[0] == "总资产 (RMB):" ||
            cells[0] == "资金可用 (RMB):" || cells[0] == "资金余额 (RMB):") {
            double value = 0.0;
            if (cells.size() > 1 && parseDouble(cells[1], value)) {
                if (cells[0] == "证券市值 (RMB):") result.securitiesMarketValue = value;
                else if (cells[0] == "总资产 (RMB):") result.totalAssets = value;
                else if (cells[0] == "资金可用 (RMB):") result.availableCash = value;
                else result.cashBalance = value;
            } else {
                result.issues.push_back("第 " + std::to_string(lineNumber) + " 行账户金额无效");
            }
            continue;
        }

        if (section == Section::Trades) {
            if (cells.size() < 11) {
                result.issues.push_back("第 " + std::to_string(lineNumber) + " 行成交字段不足");
                continue;
            }
            BrokerTradeRecord trade;
            trade.date = cells[0];
            trade.side = cells[1] == "证券买入" ? SignalType::Buy
                         : cells[1] == "证券卖出" ? SignalType::Sell : SignalType::None;
            trade.symbol = normalizeBrokerSymbol(cells[2]);
            trade.name = cells[3];
            bool valid = trade.date.size() == 8 && allDigits(trade.date) &&
                         trade.side != SignalType::None && !trade.symbol.empty() &&
                         parseQuantity(cells[4], trade.quantity) && trade.quantity > 0 &&
                         parseDouble(cells[5], trade.price) && trade.price > 0.0 &&
                         parseDouble(cells[6], trade.amount) &&
                         parseDouble(cells[7], trade.commission) &&
                         parseDouble(cells[8], trade.stampDuty) &&
                         parseDouble(cells[9], trade.transferFee) &&
                         parseDouble(cells[10], trade.balance);
            if (!valid) {
                result.issues.push_back("第 " + std::to_string(lineNumber) + " 行成交数据无效");
                continue;
            }
            result.trades.push_back(std::move(trade));
        } else if (section == Section::Positions) {
            if (cells.size() < 7) continue;
            BrokerPositionSnapshot position;
            position.market = cells[0];
            position.symbol = normalizeBrokerSymbol(cells[1]);
            position.name = cells[2];
            bool valid = !position.symbol.empty() &&
                         parseQuantity(cells[3], position.quantity) &&
                         parseDouble(cells[4], position.marketPrice) &&
                         parseDouble(cells[5], position.cost) &&
                         parseDouble(cells[6], position.marketValue);
            if (!valid) {
                result.issues.push_back("第 " + std::to_string(lineNumber) + " 行持仓数据无效");
                continue;
            }
            result.positions.push_back(std::move(position));
        }
    }

    if (result.trades.empty()) {
        result.issues.push_back("未识别到有效成交记录");
    } else {
        result.firstTradeDate = result.trades.front().date;
        result.lastTradeDate = result.trades.front().date;
        for (const auto& trade : result.trades) {
            result.firstTradeDate = std::min(result.firstTradeDate, trade.date);
            result.lastTradeDate = std::max(result.lastTradeDate, trade.date);
        }
    }
    return result;
}

BrokerStatement loadBrokerStatementCsv(const std::filesystem::path& path) {
    return parseBrokerStatementCsv(readFile(path));
}

BrokerStatement importBrokerStatementCsv(const std::filesystem::path& source,
                                         const std::filesystem::path& destination) {
    std::string content = readFile(source);
    BrokerStatement statement = parseBrokerStatementCsv(content);
    if (statement.trades.empty() && statement.positions.empty()) {
        throw std::runtime_error("所选文件不包含可识别的成交或持仓数据。");
    }
    writeTextFileAtomically(destination, content);
    return statement;
}

BrokerStatementAnalysis analyzeBrokerStatement(const BrokerStatement& statement) {
    BrokerStatementAnalysis analysis;
    analysis.tradeCount = static_cast<int>(statement.trades.size());
    std::map<std::string, std::vector<const BrokerTradeRecord*>> grouped;
    for (const auto& trade : statement.trades) {
        grouped[trade.symbol].push_back(&trade);
        double notional = trade.price * static_cast<double>(trade.quantity);
        double fees = trade.commission + trade.stampDuty + trade.transferFee;
        analysis.totalFees += fees;
        analysis.netCashFlow += trade.amount;
        if (trade.side == SignalType::Buy) {
            ++analysis.buyCount;
            analysis.buyNotional += notional;
        } else {
            ++analysis.sellCount;
            analysis.sellNotional += notional;
        }
    }

    for (const auto& [symbol, records] : grouped) {
        BrokerSymbolSummary summary;
        summary.symbol = symbol;
        summary.name = records.front()->name;
        summary.tradeCount = static_cast<int>(records.size());
        double buyValue = 0.0;
        double sellValue = 0.0;
        for (const auto* trade : records) {
            double notional = trade->price * static_cast<double>(trade->quantity);
            summary.totalFees += trade->commission + trade->stampDuty + trade->transferFee;
            summary.netCashFlow += trade->amount;
            if (trade->side == SignalType::Buy) {
                summary.buyQuantity += trade->quantity;
                buyValue += notional;
            } else {
                summary.sellQuantity += trade->quantity;
                sellValue += notional;
            }
        }
        summary.netQuantity = summary.buyQuantity - summary.sellQuantity;
        if (summary.buyQuantity > 0) summary.averageBuyPrice = buyValue / summary.buyQuantity;
        if (summary.sellQuantity > 0) summary.averageSellPrice = sellValue / summary.sellQuantity;

        struct Lot { long long quantity; double unitCost; };
        std::deque<Lot> lots;
        double matchedCost = 0.0;
        // 券商文件按日期倒序导出，从尾部向前处理可恢复时间顺序。
        for (auto iterator = records.rbegin(); iterator != records.rend(); ++iterator) {
            const BrokerTradeRecord& trade = **iterator;
            double fees = trade.commission + trade.stampDuty + trade.transferFee;
            double unitFees = fees / static_cast<double>(trade.quantity);
            if (trade.side == SignalType::Buy) {
                lots.push_back({trade.quantity, trade.price + unitFees});
                continue;
            }
            long long remaining = trade.quantity;
            double netSellPrice = trade.price - unitFees;
            while (remaining > 0 && !lots.empty()) {
                long long matched = std::min(remaining, lots.front().quantity);
                summary.matchedProfit += (netSellPrice - lots.front().unitCost) * matched;
                matchedCost += lots.front().unitCost * matched;
                summary.matchedQuantity += matched;
                remaining -= matched;
                lots.front().quantity -= matched;
                if (lots.front().quantity == 0) lots.pop_front();
            }
            summary.unmatchedSellQuantity += remaining;
        }
        summary.matchedReturnPercent = matchedCost > 0.0
                                           ? summary.matchedProfit / matchedCost * 100.0
                                           : 0.0;
        analysis.matchedProfit += summary.matchedProfit;
        analysis.symbols.push_back(std::move(summary));
    }
    std::stable_sort(analysis.symbols.begin(), analysis.symbols.end(),
                     [](const BrokerSymbolSummary& left, const BrokerSymbolSummary& right) {
                         if (left.tradeCount != right.tradeCount) return left.tradeCount > right.tradeCount;
                         return displayCode(left.symbol) < displayCode(right.symbol);
                     });
    return analysis;
}

}  // namespace ashare
