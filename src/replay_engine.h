#pragma once

#include "domain.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ashare {

class QuoteProvider;

// 行情回放引擎：管理真实历史数据缓存，并按 A 股交易分钟顺序生成当时可见的快照。
class ReplayEngine {
public:
    explicit ReplayEngine(std::filesystem::path cacheDirectory);

    // 开始新的交易日回放。调用后需要通过 ensureSymbol 加载标的数据。
    void begin(const std::string& date);
    void clear();
    bool ready() const;
    const std::string& date() const;

    // 优先读取本地历史缓存；缓存缺失时从 QuoteProvider 拉取真实数据并落盘。
    void ensureSymbol(const std::string& symbol, const std::string& name, QuoteProvider& provider,
                      int dailyLimit = 81);
    // 批量研究已预取完整日线时复用该数据，仅按交易日读取或下载分钟线。
    // 这样可显著减少多日、多标的分析时的重复 HTTP 请求。
    void ensureSymbol(const std::string& symbol, const std::string& name,
                      QuoteProvider& provider, const std::vector<KLine>& dailyHistory);
    // 接收外部授权行情源或测试夹具提供的数据，未来更换行情供应商时可复用回放逻辑。
    void installSymbolData(const std::string& symbol, const std::string& name,
                           std::vector<KLine> minuteLines,
                           std::vector<KLine> dailyLines);
    bool hasSymbol(const std::string& symbol) const;

    // cursor 从 0 到 241，分别对应 09:30-11:30 与 13:00-15:00。
    void reset();
    bool advance(int minutes = 1);
    bool finished() const;
    int cursor() const;
    int totalMinutes() const;
    std::string currentTime() const;

    std::optional<Quote> quote(const std::string& symbol) const;
    std::vector<Quote> quotes(const std::vector<std::string>& symbols) const;
    std::vector<KLine> minuteLines(const std::string& symbol) const;
    // 当天日 K 由回放进度内的分钟线聚合，避免读取当天收盘后的未来数据。
    std::vector<KLine> dailyLines(const std::string& symbol, int limit = 80) const;

private:
    struct SymbolData {
        std::string name;
        std::vector<KLine> minuteLines;
        std::vector<KLine> dailyLines;
        int dailyRequestedLimit = 81;
    };

    std::filesystem::path cachePath(const std::string& symbol, const char* kind) const;
    std::optional<std::vector<KLine>> readCache(const std::filesystem::path& path,
                                                const std::string& symbol,
                                                const char* kind) const;
    void writeCache(const std::filesystem::path& path, const std::string& symbol,
                    const char* kind, const std::vector<KLine>& lines) const;
    static int minuteIndex(const std::string& dateTime);
    void validateAndSort(const std::string& symbol, std::vector<KLine>& minuteLines,
                         std::vector<KLine>& dailyLines) const;

    std::filesystem::path cacheDirectory_;
    std::string date_;
    int cursor_ = 0;
    std::map<std::string, SymbolData> symbols_;
};

}  // namespace ashare
