#include "quote_provider.h"

#include "http_client.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ashare {

void mergeIncrementalMinuteLines(std::vector<KLine>& cached,
                                 const std::vector<KLine>& incremental,
                                 size_t maximumCount) {
    if (incremental.empty()) {
        return;
    }
    std::vector<KLine> tail = incremental;
    std::sort(tail.begin(), tail.end(), [](const KLine& left, const KLine& right) {
        return left.date < right.date;
    });
    const std::string latestDay = tail.back().date.substr(0, 10);
    tail.erase(std::remove_if(tail.begin(), tail.end(), [&](const KLine& line) {
                   return line.date.size() < 16 || line.date.compare(0, 10, latestDay) != 0;
               }),
               tail.end());
    if (tail.empty()) {
        return;
    }

    std::sort(cached.begin(), cached.end(), [](const KLine& left, const KLine& right) {
        return left.date < right.date;
    });
    if (!cached.empty()) {
        const std::string cachedDay = cached.back().date.substr(0, 10);
        if (cachedDay > latestDay) {
            return;
        }
        if (cachedDay < latestDay) {
            cached.clear();
        } else {
            cached.erase(std::remove_if(cached.begin(), cached.end(), [&](const KLine& line) {
                             return line.date.size() < 16 ||
                                    line.date.compare(0, 10, latestDay) != 0;
                         }),
                         cached.end());
        }
    }

    for (const auto& line : tail) {
        auto found = std::lower_bound(
            cached.begin(), cached.end(), line.date,
            [](const KLine& existing, const std::string& date) {
                return existing.date < date;
            });
        if (found != cached.end() && found->date == line.date) {
            *found = line;
        } else {
            cached.insert(found, line);
        }
    }
    if (maximumCount > 0 && cached.size() > maximumCount) {
        cached.erase(cached.begin(), cached.end() -
                                         static_cast<std::ptrdiff_t>(maximumCount));
    }
}

struct QuoteProvider::Impl {
public:
    std::vector<Quote> fetchQuotes(const std::vector<std::string>& symbols) const {
        if (symbols.empty()) {
            return {};
        }
        std::ostringstream query;
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (i > 0) {
                query << ",";
            }
            query << symbols[i];
        }

        // 腾讯两个域名返回相同协议，主域名不可用时自动尝试备用域名。
        const std::vector<std::string> urls = {
            "https://qt.gtimg.cn/q=" + query.str(),
            "https://web.sqt.gtimg.cn/q=" + query.str(),
        };
        bool receivedResponse = false;
        for (const auto& url : urls) {
            try {
                // 腾讯快照正文使用 GBK，解析前统一转换成 UTF-8。
                auto quotes = parseTencentQuotes(gbkToUtf8(client_.get(url)));
                receivedResponse = true;
                if (!quotes.empty()) {
                    return quotes;
                }
            } catch (const std::exception&) {
            }
        }
        // 能收到响应但没有目标记录，与完全无法连接是两种不同状态。
        if (receivedResponse) {
            return {};
        }
        throw std::runtime_error("实时行情服务连接失败，程序将继续自动重试。");
    }

    std::vector<KLine> fetchKLines(const std::string& symbol, int klt, int limit = 80) const {
        if (klt == 1) {
            // 分钟线依次使用新浪、腾讯、东方财富，任一成功即返回。
            try {
                return fetchSinaMinuteLines(symbol, limit);
            } catch (const std::exception&) {
            }
            try {
                return fetchTencentMinuteLines(symbol, limit);
            } catch (const std::exception&) {
            }
            try {
                return fetchEastmoneyKLines(symbol, klt, limit);
            } catch (const std::exception&) {
            }
            try {
                return fetchEastmoneyKLines(symbol, klt, limit);
            } catch (const std::exception&) {
            }
        }

        // 日线优先使用腾讯前复权数据，东方财富作为备用。
        try {
            return fetchTencentDailyLines(symbol, limit);
        } catch (const std::exception&) {
        }
        try {
            return fetchEastmoneyKLines(symbol, klt, limit);
        } catch (const std::exception&) {
            throw std::runtime_error("日 K 行情服务暂时不可用，程序将继续自动重试。");
        }
    }

    std::vector<KLine> fetchHistoricalMinuteLines(const std::string& symbol,
                                                  const std::string& date,
                                                  int limit) const {
        const std::string dateToken = compactDate(date);
        try {
            auto lines = fetchEastmoneyKLines(symbol, 1, limit, dateToken, dateToken);
            if (hasOpeningMinute(lines)) {
                return lines;
            }
        } catch (const std::exception&) {
        }
        try {
            auto lines = fetchTencentHistoricalMinuteLines(symbol, date, limit);
            if (hasOpeningMinute(lines)) {
                return lines;
            }
        } catch (const std::exception&) {
        }
        try {
            auto lines = fetchSinaHistoricalMinuteLines(symbol, date, limit, 1);
            if (hasOpeningMinute(lines)) {
                return lines;
            }
        } catch (const std::exception&) {
        }

        // 公开 1 分钟源通常只保留最近约 6 个交易日。研究较早日期时，
        // 使用同一行情源的真实 5 分钟 K 线兜底，避免因单一接口临时断开而丢弃整天数据。
        try {
            auto lines = fetchSinaHistoricalMinuteLines(symbol, date, limit, 5);
            if (hasOpeningMinute(lines)) {
                logger().write("INFO", "Historical minute fallback source=Sina5m symbol=" +
                                           symbol + " date=" + date + " rows=" +
                                           std::to_string(lines.size()));
                return lines;
            }
        } catch (const std::exception&) {
        }
        throw std::runtime_error("未获取到 " + symbol + " 在 " + date +
                                 " 的完整真实历史分钟行情；已依次尝试东方财富 1 分钟、"
                                 "腾讯 1 分钟、新浪 1 分钟和新浪 5 分钟备用源。");
    }

    std::vector<KLine> fetchHistoricalDailyLines(const std::string& symbol,
                                                 const std::string& endDate,
                                                 int limit) const {
        const std::string endToken = compactDate(endDate);
        try {
            return fetchEastmoneyKLines(symbol, 101, limit, endToken, "");
        } catch (const std::exception&) {
        }
        try {
            // 腾讯接口返回最近一段真实日线，过滤掉回放日之后的记录后再截取所需窗口。
            std::vector<KLine> lines = fetchTencentDailyLines(symbol, std::max(limit * 3, 240));
            lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const KLine& line) {
                            return line.date.size() < 10 || line.date.substr(0, 10) > endDate;
                        }),
                        lines.end());
            keepLatest(lines, limit);
            if (lines.empty()) {
                throw std::runtime_error("Tencent returned no historical daily data.");
            }
            return lines;
        } catch (const std::exception&) {
            throw std::runtime_error("未获取到 " + symbol + " 截止 " + endDate +
                                     " 的真实历史日线行情。");
        }
    }

private:
    static std::string compactDate(const std::string& date) {
        std::string token;
        for (char character : date) {
            if (character >= '0' && character <= '9') {
                token.push_back(character);
            }
        }
        if (token.size() != 8) {
            throw std::runtime_error("历史行情日期格式必须为 YYYY-MM-DD。");
        }
        return token;
    }

    // 历史源必须覆盖开盘阶段，避免把只剩 09:55 之后的数据当作完整交易日缓存。
    static bool hasOpeningMinute(const std::vector<KLine>& lines) {
        return std::any_of(lines.begin(), lines.end(), [](const KLine& line) {
            if (line.date.size() < 16) {
                return false;
            }
            int hour = (line.date[11] - '0') * 10 + (line.date[12] - '0');
            int minute = (line.date[14] - '0') * 10 + (line.date[15] - '0');
            return hour == 9 && minute >= 30 && minute <= 35;
        });
    }

    // 腾讯历史接口还包含 15:00 后的盘后定价记录，回放只保留连续竞价时间。
    static bool isRegularTradingMinute(const std::string& hourMinute) {
        if (hourMinute.size() != 4 ||
            !std::all_of(hourMinute.begin(), hourMinute.end(), [](char value) {
                return value >= '0' && value <= '9';
            })) {
            return false;
        }
        int value = ((hourMinute[0] - '0') * 10 + (hourMinute[1] - '0')) * 60 +
                    (hourMinute[2] - '0') * 10 + (hourMinute[3] - '0');
        return (value >= 9 * 60 + 30 && value <= 11 * 60 + 30) ||
               (value >= 13 * 60 && value <= 15 * 60);
    }

    static std::vector<Quote> parseTencentQuotes(const std::string& body) {
        std::vector<Quote> quotes;
        static const std::regex lineRegex(R"REGEX(v_([a-z]{2}\d{6})="([^"]*)")REGEX");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), lineRegex); it != std::sregex_iterator(); ++it) {
            auto fields = split((*it)[2].str(), '~');
            if (fields.size() < 6) {
                continue;
            }
            Quote quote;
            quote.symbol = (*it)[1].str();
            quote.name = fields.size() > 1 ? fields[1] : "";
            quote.price = fields.size() > 3 ? parseDouble(fields[3]) : 0.0;
            quote.previousClose = fields.size() > 4 ? parseDouble(fields[4]) : 0.0;
            quote.open = fields.size() > 5 ? parseDouble(fields[5]) : 0.0;
            // 腾讯成交量以“手”返回，内部统一换算为“股”；成交额换算为元。
            quote.volume = fields.size() > 36 ? parseDouble(fields[36]) * 100.0 : (fields.size() > 6 ? parseDouble(fields[6]) * 100.0 : 0.0);
            quote.amount = fields.size() > 37 ? parseDouble(fields[37]) * 10000.0 : 0.0;
            quote.time = fields.size() > 30 ? fields[30] : "";
            quote.high = fields.size() > 33 ? parseDouble(fields[33]) : 0.0;
            quote.low = fields.size() > 34 ? parseDouble(fields[34]) : 0.0;
            quote.turnoverRate = fields.size() > 38 ? parseDouble(fields[38]) : 0.0;
            // 先按价格自行计算；响应包含官方涨跌字段时优先采用官方值。
            quote.change = quote.price - quote.previousClose;
            quote.changePercent = quote.previousClose == 0.0 ? 0.0 : quote.change / quote.previousClose * 100.0;
            if (fields.size() > 32) {
                quote.change = parseDouble(fields[31]);
                quote.changePercent = parseDouble(fields[32]);
            }
            quotes.push_back(quote);
        }
        return quotes;
    }

    static void keepLatest(std::vector<KLine>& lines, int limit) {
        if (limit > 0 && lines.size() > static_cast<size_t>(limit)) {
            lines.erase(lines.begin(), lines.end() - limit);
        }
    }

    static void keepLatestTradingDay(std::vector<KLine>& lines) {
        if (lines.empty() || lines.back().date.size() < 10) {
            return;
        }
        // 某些接口会混入上一交易日尾部数据，只保留响应中的最新交易日。
        std::string latestDate = lines.back().date.substr(0, 10);
        lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const KLine& line) {
                        return line.date.size() < 10 || line.date.compare(0, 10, latestDate) != 0;
                    }),
                    lines.end());
    }

    std::vector<KLine> fetchEastmoneyKLines(const std::string& symbol, int klt, int limit,
                                            const std::string& end = "20500101",
                                            const std::string& begin = "") const {
        std::ostringstream url;
        url << "https://push2his.eastmoney.com/api/qt/stock/kline/get"
            << "?secid=" << eastmoneySecid(symbol)
            << "&ut=7eea3edcaed734bea9cbfc24409ed989"
            << "&fields1=f1,f2,f3,f4,f5,f6"
            << "&fields2=f51,f52,f53,f54,f55,f56"
            << "&klt=" << klt << "&fqt=1&end=" << end << "&lmt=" << limit;
        if (!begin.empty()) {
            url << "&beg=" << begin;
        }
        url
            // 时间戳用于绕过中间缓存，避免分钟线长时间停留在旧响应。
            << "&_=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();

        std::string body = client_.get(url.str());
        std::vector<KLine> lines;
        static const std::regex klineRegex(R"REGEX("(\d{4}-\d{2}-\d{2}(?: \d{2}:\d{2})?,[^"]+)")REGEX");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), klineRegex); it != std::sregex_iterator(); ++it) {
            auto fields = split((*it)[1].str(), ',');
            if (fields.size() < 6) {
                continue;
            }
            KLine line;
            line.date = fields[0];
            line.open = parseDouble(fields[1]);
            line.close = parseDouble(fields[2]);
            line.high = parseDouble(fields[3]);
            line.low = parseDouble(fields[4]);
            line.volume = parseDouble(fields[5]);
            lines.push_back(line);
        }
        if (klt == 1 && begin.empty()) {
            keepLatestTradingDay(lines);
        }
        if (klt == 1 && !begin.empty()) {
            const std::string expectedDate = begin.substr(0, 4) + "-" + begin.substr(4, 2) + "-" + begin.substr(6, 2);
            lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const KLine& line) {
                            return line.date.size() < 10 || line.date.compare(0, 10, expectedDate) != 0;
                        }),
                        lines.end());
        }
        keepLatest(lines, limit);
        if (lines.empty()) {
            throw std::runtime_error("Eastmoney returned no K-line data.");
        }
        return lines;
    }

    std::vector<KLine> fetchTencentHistoricalMinuteLines(const std::string& symbol,
                                                          const std::string& date,
                                                          int limit) const {
        std::string url = "https://web.ifzq.gtimg.cn/appstock/app/day/query?code=" + symbol +
                          "&date=" + compactDate(date);
        std::string body = client_.get(url);

        // 该接口一次返回多个交易日，必须先截取目标日期的 data 数组。
        // 旧实现直接扫描整个响应并统一标记为请求日期，截断后会丢失开盘前 25 分钟。
        const std::string dateToken = compactDate(date);
        const std::string groupMarker = "\"date\":\"" + dateToken + "\",\"data\":[";
        size_t groupBegin = body.find(groupMarker);
        if (groupBegin == std::string::npos) {
            throw std::runtime_error("Tencent response has no requested historical date.");
        }
        groupBegin += groupMarker.size();
        size_t groupEnd = body.find(']', groupBegin);
        if (groupEnd == std::string::npos) {
            throw std::runtime_error("Tencent historical minute group is incomplete.");
        }
        std::string group = body.substr(groupBegin, groupEnd - groupBegin);

        std::vector<KLine> lines;
        static const std::regex minuteRegex(
            R"REGEX("(\d{2})(\d{2}) ([+-]?\d+(?:\.\d+)?) ([+-]?\d+(?:\.\d+)?) [+-]?\d+(?:\.\d+)?")REGEX");
        double previousPrice = 0.0;
        double previousVolume = 0.0;
        for (auto it = std::sregex_iterator(group.begin(), group.end(), minuteRegex);
             it != std::sregex_iterator(); ++it) {
            const std::string hourMinute = (*it)[1].str() + (*it)[2].str();
            if (!isRegularTradingMinute(hourMinute)) {
                continue;
            }
            double price = parseDouble((*it)[3].str());
            double cumulativeVolume = parseDouble((*it)[4].str());
            KLine line;
            line.date = date + " " + (*it)[1].str() + ":" + (*it)[2].str();
            line.open = previousPrice > 0.0 ? previousPrice : price;
            line.close = price;
            line.high = std::max(line.open, line.close);
            line.low = std::min(line.open, line.close);
            line.volume = std::max(0.0, cumulativeVolume - previousVolume);
            lines.push_back(line);
            previousPrice = price;
            previousVolume = cumulativeVolume;
        }
        keepLatest(lines, limit);
        if (lines.empty()) {
            throw std::runtime_error("Tencent returned no historical minute data.");
        }
        return lines;
    }

    std::vector<KLine> fetchSinaHistoricalMinuteLines(const std::string& symbol,
                                                      const std::string& date,
                                                      int limit,
                                                      int scaleMinutes) const {
        std::ostringstream url;
        url << "https://quotes.sina.cn/cn/api/json_v2.php/CN_MarketDataService.getKLineData"
            << "?symbol=" << symbol << "&scale=" << scaleMinutes
            << "&ma=no&datalen=1500"
            << "&_=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
        std::string body = client_.get(url.str());

        std::vector<KLine> lines;
        static const std::regex minuteRegex(
            R"REGEX(\{"day":"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}):\d{2}","open":"([^"]+)","high":"([^"]+)","low":"([^"]+)","close":"([^"]+)","volume":"([^"]+)")REGEX");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), minuteRegex);
             it != std::sregex_iterator(); ++it) {
            if ((*it)[1].str().compare(0, 10, date) != 0) {
                continue;
            }
            KLine line;
            line.date = (*it)[1].str();
            line.open = parseDouble((*it)[2].str());
            line.high = parseDouble((*it)[3].str());
            line.low = parseDouble((*it)[4].str());
            line.close = parseDouble((*it)[5].str());
            line.volume = parseDouble((*it)[6].str());
            lines.push_back(line);
        }
        keepLatest(lines, limit);
        if (lines.empty()) {
            throw std::runtime_error("Sina returned no historical " +
                                     std::to_string(scaleMinutes) + "-minute data.");
        }
        return lines;
    }

    std::vector<KLine> fetchTencentMinuteLines(const std::string& symbol, int limit) const {
        std::string url = "https://web.ifzq.gtimg.cn/appstock/app/minute/query?code=" + symbol;
        std::string body = client_.get(url);

        std::smatch dateMatch;
        static const std::regex dateRegex(R"REGEX("date":"?(\d{4})(\d{2})(\d{2})"?")REGEX");
        if (!std::regex_search(body, dateMatch, dateRegex)) {
            throw std::runtime_error("Tencent minute response has no date.");
        }
        std::string date = dateMatch[1].str() + "-" + dateMatch[2].str() + "-" + dateMatch[3].str();

        std::vector<KLine> lines;
        static const std::regex minuteRegex(R"REGEX("(\d{2})(\d{2}) ([+-]?\d+(?:\.\d+)?) ([+-]?\d+(?:\.\d+)?) [+-]?\d+(?:\.\d+)?")REGEX");
        // 腾讯分钟接口给出价格和累计成交量，这里还原为每分钟增量成交量。
        double previousPrice = 0.0;
        double previousVolume = 0.0;
        for (auto it = std::sregex_iterator(body.begin(), body.end(), minuteRegex); it != std::sregex_iterator(); ++it) {
            double price = parseDouble((*it)[3].str());
            double cumulativeVolume = parseDouble((*it)[4].str());
            KLine line;
            line.date = date + " " + (*it)[1].str() + ":" + (*it)[2].str();
            line.open = previousPrice > 0.0 ? previousPrice : price;
            line.close = price;
            line.high = std::max(line.open, line.close);
            line.low = std::min(line.open, line.close);
            line.volume = std::max(0.0, cumulativeVolume - previousVolume);
            lines.push_back(line);
            previousPrice = price;
            previousVolume = cumulativeVolume;
        }
        keepLatestTradingDay(lines);
        keepLatest(lines, limit);
        if (lines.empty()) {
            throw std::runtime_error("Tencent returned no minute data.");
        }
        return lines;
    }

    std::vector<KLine> fetchSinaMinuteLines(const std::string& symbol, int limit) const {
        std::ostringstream url;
        url << "https://quotes.sina.cn/cn/api/json_v2.php/CN_MarketDataService.getKLineData"
            << "?symbol=" << symbol << "&scale=1&ma=no&datalen=" << limit
            << "&_=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
        std::string body = client_.get(url.str());

        std::vector<KLine> lines;
        // 新浪直接提供每分钟 OHLCV，解析后可直接映射到统一 KLine 结构。
        static const std::regex minuteRegex(
            R"REGEX(\{"day":"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}):\d{2}","open":"([^"]+)","high":"([^"]+)","low":"([^"]+)","close":"([^"]+)","volume":"([^"]+)")REGEX");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), minuteRegex); it != std::sregex_iterator(); ++it) {
            KLine line;
            line.date = (*it)[1].str();
            line.open = parseDouble((*it)[2].str());
            line.high = parseDouble((*it)[3].str());
            line.low = parseDouble((*it)[4].str());
            line.close = parseDouble((*it)[5].str());
            line.volume = parseDouble((*it)[6].str());
            lines.push_back(line);
        }
        keepLatestTradingDay(lines);
        keepLatest(lines, limit);
        if (lines.empty()) {
            throw std::runtime_error("Sina returned no minute data.");
        }
        return lines;
    }

    std::vector<KLine> fetchTencentDailyLines(const std::string& symbol, int limit) const {
        std::ostringstream url;
        // qfq 表示前复权，便于策略比较跨除权日的历史价格。
        url << "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param="
            << symbol << ",day,,," << limit << ",qfq";
        std::string body = client_.get(url.str());

        std::vector<KLine> lines;
        static const std::regex dailyRegex(R"REGEX(\["(\d{4}-\d{2}-\d{2})","([^"]+)","([^"]+)","([^"]+)","([^"]+)","([^"]+)")REGEX");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), dailyRegex); it != std::sregex_iterator(); ++it) {
            KLine line;
            line.date = (*it)[1].str();
            line.open = parseDouble((*it)[2].str());
            line.close = parseDouble((*it)[3].str());
            line.high = parseDouble((*it)[4].str());
            line.low = parseDouble((*it)[5].str());
            line.volume = parseDouble((*it)[6].str());
            lines.push_back(line);
        }
        keepLatest(lines, limit);
        if (lines.empty()) {
            throw std::runtime_error("Tencent returned no daily data.");
        }
        return lines;
    }

    WinHttpClient client_;
};

QuoteProvider::QuoteProvider() : impl_(std::make_unique<Impl>()) {}
QuoteProvider::~QuoteProvider() = default;
QuoteProvider::QuoteProvider(QuoteProvider&&) noexcept = default;
QuoteProvider& QuoteProvider::operator=(QuoteProvider&&) noexcept = default;

std::vector<Quote> QuoteProvider::fetchQuotes(const std::vector<std::string>& symbols) const {
    return impl_->fetchQuotes(symbols);
}

std::vector<KLine> QuoteProvider::fetchKLines(const std::string& symbol, int klt, int limit) const {
    return impl_->fetchKLines(symbol, klt, limit);
}

std::vector<KLine> QuoteProvider::fetchHistoricalMinuteLines(const std::string& symbol,
                                                             const std::string& date,
                                                             int limit) const {
    return impl_->fetchHistoricalMinuteLines(symbol, date, limit);
}

std::vector<KLine> QuoteProvider::fetchHistoricalDailyLines(const std::string& symbol,
                                                            const std::string& endDate,
                                                            int limit) const {
    return impl_->fetchHistoricalDailyLines(symbol, endDate, limit);
}

}  // namespace ashare
