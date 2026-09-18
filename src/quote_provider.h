#pragma once

#include "domain.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ashare {

// 将行情源返回的分钟尾部合并进缓存：同一分钟覆盖、跨日重置，并限制最大保留数量。
void mergeIncrementalMinuteLines(std::vector<KLine>& cached,
                                 const std::vector<KLine>& incremental,
                                 size_t maximumCount = 242);

// 行情聚合层：屏蔽腾讯、东方财富和新浪接口差异，并负责数据源降级。
class QuoteProvider {
public:
    QuoteProvider();
    ~QuoteProvider();
    QuoteProvider(const QuoteProvider&) = delete;
    QuoteProvider& operator=(const QuoteProvider&) = delete;
    QuoteProvider(QuoteProvider&&) noexcept;
    QuoteProvider& operator=(QuoteProvider&&) noexcept;

    // 批量获取实时快照，返回顺序与行情源响应有关，调用方按 symbol 归并。
    std::vector<Quote> fetchQuotes(const std::vector<std::string>& symbols) const;
    // klt=1 获取当日分钟线，klt=101 获取复权日线。
    std::vector<KLine> fetchKLines(const std::string& symbol, int klt, int limit = 80) const;
    // 获取指定交易日的真实历史分钟线；三个公开数据源依次降级，均失败时抛出异常。
    std::vector<KLine> fetchHistoricalMinuteLines(const std::string& symbol,
                                                  const std::string& date,
                                                  int limit = 242) const;
    // 获取截止指定交易日的历史日线，供回放构造昨收和历史指标。
    std::vector<KLine> fetchHistoricalDailyLines(const std::string& symbol,
                                                 const std::string& endDate,
                                                 int limit = 80) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ashare
