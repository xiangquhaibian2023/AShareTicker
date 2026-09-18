#include "replay_engine.h"

#include "quote_provider.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ashare {
namespace {

constexpr int replayMinuteCount = 242;

bool validPriceLine(const KLine& line) {
    return std::isfinite(line.open) && std::isfinite(line.close) &&
           std::isfinite(line.high) && std::isfinite(line.low) &&
           std::isfinite(line.volume) && line.open > 0.0 && line.close > 0.0 &&
           line.high >= std::max(line.open, line.close) &&
           line.low <= std::min(line.open, line.close) && line.low > 0.0 &&
           line.volume >= 0.0;
}

std::string compactDate(const std::string& date) {
    std::string token;
    for (char character : date) {
        if (character >= '0' && character <= '9') {
            token.push_back(character);
        }
    }
    return token;
}

std::optional<std::string> minuteCacheIssue(
    const std::vector<KLine>& lines, const std::string& replayDate) {
    bool containsOpening = false;
    bool containsClosing = false;
    for (const auto& line : lines) {
        if (line.date.size() < 16) continue;
        const std::string time = line.date.substr(11, 5);
        containsOpening = containsOpening ||
                          (time >= "09:30" && time <= "09:35");
        containsClosing = containsClosing ||
                          (time >= "14:55" && time <= "15:00");
    }
    if (!containsOpening) {
        return "missing opening minutes";
    }
    // 当天回放允许缓存只到当前分钟；已结束的历史交易日必须覆盖收盘前五分钟，
    // 否则早盘生成的缓存会永久漏掉午后行情并扭曲跨日回测结果。
    if (compactDate(replayDate) < currentDateToken() && !containsClosing) {
        return "historical cache ends before 14:55";
    }
    return std::nullopt;
}

}  // namespace

ReplayEngine::ReplayEngine(std::filesystem::path cacheDirectory)
    : cacheDirectory_(std::move(cacheDirectory)) {}

void ReplayEngine::begin(const std::string& date) {
    static const std::regex datePattern(R"(^\d{4}-\d{2}-\d{2}$)");
    if (!std::regex_match(date, datePattern)) {
        throw std::runtime_error("回放日期格式必须为 YYYY-MM-DD。");
    }
    date_ = date;
    cursor_ = 0;
    symbols_.clear();
}

void ReplayEngine::clear() {
    date_.clear();
    cursor_ = 0;
    symbols_.clear();
}

bool ReplayEngine::ready() const {
    return !date_.empty() && !symbols_.empty();
}

const std::string& ReplayEngine::date() const {
    return date_;
}

std::filesystem::path ReplayEngine::cachePath(const std::string& symbol, const char* kind) const {
    return cacheDirectory_ / std::filesystem::path(compactDate(date_)) /
           std::filesystem::path(symbol + "_" + kind + ".tsv");
}

std::optional<std::vector<KLine>> ReplayEngine::readCache(const std::filesystem::path& path,
                                                          const std::string& symbol,
                                                          const char* kind) const {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }
    std::vector<KLine> lines;
    std::string line;
    bool metadataMatches = false;
    while (std::getline(input, line)) {
        if (line.rfind("# symbol=", 0) == 0) {
            metadataMatches = line.substr(9) == symbol;
            continue;
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        auto fields = split(line, '\t');
        if (fields.size() != 6) {
            return std::nullopt;
        }
        try {
            KLine item;
            item.date = fields[0];
            item.open = std::stod(fields[1]);
            item.close = std::stod(fields[2]);
            item.high = std::stod(fields[3]);
            item.low = std::stod(fields[4]);
            item.volume = std::stod(fields[5]);
            lines.push_back(std::move(item));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (!metadataMatches || lines.empty()) {
        return std::nullopt;
    }
    logger().write("INFO", "Replay cache hit symbol=" + symbol + " kind=" + kind +
                               " rows=" + std::to_string(lines.size()));
    return lines;
}

void ReplayEngine::writeCache(const std::filesystem::path& path, const std::string& symbol,
                              const char* kind, const std::vector<KLine>& lines) const {
    std::ostringstream output;
    output << "# AShareTicker real historical market data cache\n"
           << "# symbol=" << symbol << "\n"
           << "# date=" << date_ << "\n"
           << "# kind=" << kind << "\n"
           << std::setprecision(12);
    for (const auto& line : lines) {
        output << line.date << '\t' << line.open << '\t' << line.close << '\t'
               << line.high << '\t' << line.low << '\t' << line.volume << '\n';
    }
    writeTextFileAtomically(path, output.str());
}

void ReplayEngine::ensureSymbol(const std::string& symbol, const std::string& name,
                                QuoteProvider& provider, int dailyLimit) {
    auto existing = symbols_.find(symbol);
    if (existing != symbols_.end()) {
        if (!name.empty()) {
            existing->second.name = name;
        }
        if (dailyLimit > existing->second.dailyRequestedLimit) {
            existing->second.dailyRequestedLimit = dailyLimit;
            if (existing->second.dailyLines.size() < static_cast<size_t>(dailyLimit)) {
                try {
                    auto extended = provider.fetchHistoricalDailyLines(symbol, date_, dailyLimit);
                    validateAndSort(symbol, existing->second.minuteLines, extended);
                    if (extended.size() > existing->second.dailyLines.size()) {
                        existing->second.dailyLines = std::move(extended);
                        writeCache(cachePath(symbol, "daily"), symbol, "daily", existing->second.dailyLines);
                    }
                } catch (const std::exception& error) {
                    logger().write("WARN", "Technical swing replay extension failed: " + std::string(error.what()));
                }
            }
        }
        return;
    }
    if (date_.empty()) {
        throw std::runtime_error("请先设置回放日期。");
    }

    auto minutePath = cachePath(symbol, "minute");
    auto dailyPath = cachePath(symbol, "daily");
    auto cachedMinute = readCache(minutePath, symbol, "minute");
    auto cachedDaily = readCache(dailyPath, symbol, "daily");
    if (cachedDaily && dailyLimit > 81 && cachedDaily->size() < static_cast<size_t>(dailyLimit)) {
        try {
            auto extended = provider.fetchHistoricalDailyLines(symbol, date_, dailyLimit);
            if (extended.size() > cachedDaily->size()) {
                cachedDaily = std::move(extended);
                writeCache(dailyPath, symbol, "daily", *cachedDaily);
            }
        } catch (const std::exception& error) {
            logger().write("WARN", "Technical swing history extension failed: " + std::string(error.what()));
        }
    }
    if (cachedMinute) {
        if (auto issue = minuteCacheIssue(*cachedMinute, date_)) {
            logger().write("WARN", "Discarding incomplete replay minute cache symbol=" + symbol +
                                       " date=" + date_ + " reason=" + *issue);
            cachedMinute.reset();
        }
    }
    std::vector<KLine> minute = cachedMinute ? std::move(*cachedMinute)
                                             : provider.fetchHistoricalMinuteLines(symbol, date_, 242);
    std::vector<KLine> daily = cachedDaily ? std::move(*cachedDaily)
                                           : provider.fetchHistoricalDailyLines(symbol, date_, dailyLimit);
    validateAndSort(symbol, minute, daily);
    if (!cachedMinute) {
        writeCache(minutePath, symbol, "minute", minute);
    }
    if (!cachedDaily) {
        writeCache(dailyPath, symbol, "daily", daily);
    }
    installSymbolData(symbol, name, std::move(minute), std::move(daily));
    symbols_.at(symbol).dailyRequestedLimit = dailyLimit;
    logger().write("INFO", "Replay data loaded symbol=" + symbol + " date=" + date_);
}

void ReplayEngine::ensureSymbol(const std::string& symbol, const std::string& name,
                                QuoteProvider& provider,
                                const std::vector<KLine>& dailyHistory) {
    auto existing = symbols_.find(symbol);
    if (existing != symbols_.end()) {
        if (!name.empty()) {
            existing->second.name = name;
        }
        return;
    }
    if (date_.empty()) {
        throw std::runtime_error("请先设置回放日期。");
    }
    auto minutePath = cachePath(symbol, "minute");
    auto cachedMinute = readCache(minutePath, symbol, "minute");
    if (cachedMinute) {
        if (auto issue = minuteCacheIssue(*cachedMinute, date_)) {
            logger().write("WARN", "Discarding incomplete research minute cache symbol=" +
                                       symbol + " date=" + date_ + " reason=" + *issue);
            cachedMinute.reset();
        }
    }
    std::vector<KLine> minute = cachedMinute
                                    ? std::move(*cachedMinute)
                                    : provider.fetchHistoricalMinuteLines(symbol, date_, 242);
    std::vector<KLine> daily = dailyHistory;
    validateAndSort(symbol, minute, daily);
    if (!cachedMinute) {
        writeCache(minutePath, symbol, "minute", minute);
    }
    symbols_[symbol] = SymbolData{name.empty() ? symbol : name,
                                  std::move(minute), std::move(daily)};
    logger().write("INFO", "Research replay data loaded symbol=" + symbol +
                               " date=" + date_);
}

void ReplayEngine::installSymbolData(const std::string& symbol, const std::string& name,
                                     std::vector<KLine> minuteLines,
                                     std::vector<KLine> dailyLines) {
    if (date_.empty()) {
        throw std::runtime_error("请先设置回放日期。");
    }
    validateAndSort(symbol, minuteLines, dailyLines);
    symbols_[symbol] = SymbolData{name.empty() ? symbol : name,
                                  std::move(minuteLines), std::move(dailyLines)};
}

bool ReplayEngine::hasSymbol(const std::string& symbol) const {
    return symbols_.find(symbol) != symbols_.end();
}

void ReplayEngine::reset() {
    cursor_ = 0;
    // 部分行情源的第一根分钟线是 09:31。重置到所有已加载标的都具备快照的
    // 最早分钟，避免固定停在 09:30 时列表和图表为空。
    for (const auto& [symbol, data] : symbols_) {
        (void)symbol;
        if (!data.minuteLines.empty()) {
            cursor_ = std::max(cursor_, minuteIndex(data.minuteLines.front().date));
        }
    }
}

bool ReplayEngine::advance(int minutes) {
    if (minutes <= 0 || finished()) {
        return false;
    }
    cursor_ = std::min(replayMinuteCount - 1, cursor_ + minutes);
    return true;
}

bool ReplayEngine::finished() const {
    return cursor_ >= replayMinuteCount - 1;
}

int ReplayEngine::cursor() const {
    return cursor_;
}

int ReplayEngine::totalMinutes() const {
    return replayMinuteCount;
}

std::string ReplayEngine::currentTime() const {
    int total = cursor_ <= 120 ? 9 * 60 + 30 + cursor_ : 13 * 60 + cursor_ - 121;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << total / 60 << ':'
           << std::setw(2) << total % 60;
    return output.str();
}

int ReplayEngine::minuteIndex(const std::string& dateTime) {
    auto separator = dateTime.rfind(' ');
    if (separator == std::string::npos || separator + 5 >= dateTime.size()) {
        return -1;
    }
    const char* time = dateTime.c_str() + separator + 1;
    if (time[0] < '0' || time[0] > '9' || time[1] < '0' || time[1] > '9' ||
        time[2] != ':' || time[3] < '0' || time[3] > '9' ||
        time[4] < '0' || time[4] > '9') {
        return -1;
    }
    int value = ((time[0] - '0') * 10 + (time[1] - '0')) * 60 +
                (time[3] - '0') * 10 + (time[4] - '0');
    constexpr int morningOpen = 9 * 60 + 30;
    constexpr int morningClose = 11 * 60 + 30;
    constexpr int afternoonOpen = 13 * 60;
    constexpr int afternoonClose = 15 * 60;
    if (value >= morningOpen && value <= morningClose) {
        return value - morningOpen;
    }
    if (value >= afternoonOpen && value <= afternoonClose) {
        return 121 + value - afternoonOpen;
    }
    return -1;
}

void ReplayEngine::validateAndSort(const std::string& symbol,
                                   std::vector<KLine>& minuteLines,
                                   std::vector<KLine>& dailyLines) const {
    minuteLines.erase(std::remove_if(minuteLines.begin(), minuteLines.end(), [&](const KLine& line) {
                          return line.date.size() < 16 || line.date.compare(0, 10, date_) != 0 ||
                                 minuteIndex(line.date) < 0 || !validPriceLine(line);
                      }),
                      minuteLines.end());
    std::sort(minuteLines.begin(), minuteLines.end(), [](const KLine& left, const KLine& right) {
        return minuteIndex(left.date) < minuteIndex(right.date);
    });
    minuteLines.erase(std::unique(minuteLines.begin(), minuteLines.end(), [](const KLine& left, const KLine& right) {
                          return minuteIndex(left.date) == minuteIndex(right.date);
                      }),
                      minuteLines.end());

    dailyLines.erase(std::remove_if(dailyLines.begin(), dailyLines.end(), [&](const KLine& line) {
                         return line.date.size() < 10 || line.date.substr(0, 10) > date_ || !validPriceLine(line);
                     }),
                     dailyLines.end());
    std::sort(dailyLines.begin(), dailyLines.end(), [](const KLine& left, const KLine& right) {
        return left.date < right.date;
    });
    dailyLines.erase(std::unique(dailyLines.begin(), dailyLines.end(), [](const KLine& left, const KLine& right) {
                         return left.date.substr(0, 10) == right.date.substr(0, 10);
                     }),
                     dailyLines.end());

    if (minuteLines.empty()) {
        throw std::runtime_error(symbol + " 在 " + date_ + " 没有有效的真实分钟行情。");
    }
    if (minuteIndex(minuteLines.front().date) > 5) {
        throw std::runtime_error(symbol + " 在 " + date_ +
                                 " 的分钟行情缺少开盘阶段，已拒绝用于回放。");
    }
    bool hasPriorClose = std::any_of(dailyLines.begin(), dailyLines.end(), [&](const KLine& line) {
        return line.date.substr(0, 10) < date_;
    });
    if (!hasPriorClose) {
        throw std::runtime_error(symbol + " 缺少回放日前一交易日的日线数据。");
    }
}

std::vector<KLine> ReplayEngine::minuteLines(const std::string& symbol) const {
    auto found = symbols_.find(symbol);
    if (found == symbols_.end()) {
        return {};
    }
    std::vector<KLine> visible;
    for (const auto& line : found->second.minuteLines) {
        if (minuteIndex(line.date) <= cursor_) {
            visible.push_back(line);
        }
    }
    return visible;
}

std::optional<Quote> ReplayEngine::quote(const std::string& symbol) const {
    auto found = symbols_.find(symbol);
    if (found == symbols_.end()) {
        return std::nullopt;
    }
    std::vector<KLine> visible = minuteLines(symbol);
    if (visible.empty()) {
        return std::nullopt;
    }
    double previousClose = 0.0;
    for (const auto& line : found->second.dailyLines) {
        if (line.date.substr(0, 10) < date_) {
            previousClose = line.close;
        }
    }
    if (previousClose <= 0.0) {
        previousClose = visible.front().open;
    }
    Quote result;
    result.symbol = symbol;
    result.name = found->second.name;
    result.price = visible.back().close;
    result.previousClose = previousClose;
    result.open = visible.front().open;
    result.change = result.price - result.previousClose;
    result.changePercent = result.previousClose > 0.0 ? result.change / result.previousClose * 100.0 : 0.0;
    result.time = visible.back().date;
    for (const auto& line : visible) {
        // K 线成交量通常以手计，列表统一换算成股并按成交价估算成交额。
        result.volume += line.volume * 100.0;
        result.amount += line.volume * 100.0 * line.close;
    }
    return result;
}

std::vector<Quote> ReplayEngine::quotes(const std::vector<std::string>& symbols) const {
    std::vector<Quote> result;
    result.reserve(symbols.size());
    for (const auto& symbol : symbols) {
        if (auto item = quote(symbol)) {
            result.push_back(std::move(*item));
        }
    }
    return result;
}

std::vector<KLine> ReplayEngine::dailyLines(const std::string& symbol, int limit) const {
    auto found = symbols_.find(symbol);
    if (found == symbols_.end()) {
        return {};
    }
    std::vector<KLine> result;
    for (const auto& line : found->second.dailyLines) {
        if (line.date.substr(0, 10) < date_) {
            result.push_back(line);
        }
    }
    std::vector<KLine> visible = minuteLines(symbol);
    if (!visible.empty()) {
        KLine partial;
        partial.date = date_;
        partial.open = visible.front().open;
        partial.close = visible.back().close;
        partial.high = visible.front().high;
        partial.low = visible.front().low;
        for (const auto& line : visible) {
            partial.high = std::max(partial.high, line.high);
            partial.low = std::min(partial.low, line.low);
            partial.volume += line.volume;
        }
        result.push_back(partial);
    }
    if (limit > 0 && result.size() > static_cast<size_t>(limit)) {
        result.erase(result.begin(), result.end() - limit);
    }
    return result;
}

}  // namespace ashare
